// Copyright (c) Athena Dev Teams - Licensed under GNU GPL
// For more information, see LICENCE in the main folder

#include "../common/mmo.h"
#include "../common/db.h"
#include "../common/malloc.h"
#include "../common/strlib.h"
#include "../common/showmsg.h"
#include "../common/socket.h"
#include "../common/timer.h"
#include "char.h"
#include "inter.h"
#include "int_party.h"
#include "int_guild.h"
#include "int_storage.h"
#include "int_pet.h"
#include "int_homun.h"
#include "int_mercenary.h"
#include "int_mail.h"
#include "int_auction.h"
#include "int_quest.h"
#include "int_elemental.h"
#include "int_clan.h"
#include "int_achievement.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

#include <sys/stat.h> //For stat/lstat/fstat - [Dekamaster/Ultimate GM Tool]

#define WISDATA_TTL (60 * 1000) //Wis data Time To Live (60 seconds)
#define WISDELLIST_MAX 256 //Number of elements in the list Delete data Wis

Sql *sql_handle = NULL; //Link to mysql db, connection FD

int char_server_port = 3306;
char char_server_ip[64] = "127.0.0.1";
char char_server_id[32] = "ragnarok";
char char_server_pw[32] = "";
char char_server_db[32] = "ragnarok";
char default_codepage[32] = ""; // Feature by irmin

struct Inter_Config interserv_config;
static struct accreg *accreg_pt;
unsigned int party_share_level = 15;

// Received packet Lengths from map-server
int inter_recv_packet_length[] = {
	-1,-1, 7,-1, -1,13,36, (2 + 4 + 4 + 4 + NAME_LENGTH), 0, -1, 0, 0, 0, 0, 0, 0, // 3000-
	 6,-1, 0, 0,  0, 0, 0, 0, 10,-1, 0, 0,  0, 0,  0, 0, // 3010-
	-1,10,-1,14,15 + NAME_LENGTH,19, 6,-1, 14,14, 6, 0,  0, 0,  0, 0, // 3020- Party
	-1, 6,-1,-1, 55,19, 6,-1, 14,-1,-1,-1, 18,19,186,-1, // 3030-
	-1, 9, 0, 0,  0, 0, 0, 0,  8, 6,11,10, 10,-1,6 + NAME_LENGTH, 0, // 3040-
	-1,-1,10,10,  0,-1,12, 0,  0, 0, 0, 0,  0, 0,  0, 0, // 3050-  Auction System [Zephyrus]
	 6,-1, 6,-1,16 + NAME_LENGTH + ACHIEVEMENT_NAME_LENGTH, 0, 0, 0,  0, 0, 0, 0,  0, 0,  0, 0, // 3060-  Quest system [Kevin] [Inkfish] / Achievements [Aleos]
	-1,10, 6,-1,  0, 0, 0, 0,  0, 0,-1,12, -1,10,  6,-1, // 3070-  Mercenary packets [Zephyrus], Elemental packets [pakpil]
	48,14,-1, 6,  0, 0, 0, 0,  0, 0,13,-1,  0, 0,  0, 0, // 3080-  Pet System, Storage
	-1,10,-1, 6,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0,  0, 0, // 3090-  Homunculus packets [albator]
	 2,-1, 6, 6,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0,  0, 0, // 30A0-  Clan packets
};

struct WisData {
	int id, fd, count, len;
	unsigned long tick;
	unsigned char src[NAME_LENGTH], dst[NAME_LENGTH], msg[512];
};
static DBMap *wis_db = NULL; // int wis_id -> struct WisData*
static int wis_dellist[WISDELLIST_MAX], wis_delnum;

/* From pc.c due to @accinfo. any ideas to replace this crap are more than welcome. */
const char *job_name(int class_) {
	switch( class_ ) {
		case JOB_NOVICE:
		case JOB_SWORDMAN:
		case JOB_MAGE:
		case JOB_ARCHER:
		case JOB_ACOLYTE:
		case JOB_MERCHANT:
		case JOB_THIEF:
			return msg_txt(JOB_NOVICE + class_);

		case JOB_KNIGHT:
		case JOB_PRIEST:
		case JOB_WIZARD:
		case JOB_BLACKSMITH:
		case JOB_HUNTER:
		case JOB_ASSASSIN:
			return msg_txt(7 - JOB_KNIGHT + class_);

		case JOB_KNIGHT2:
			return msg_txt(7);

		case JOB_CRUSADER:
		case JOB_MONK:
		case JOB_SAGE:
		case JOB_ROGUE:
		case JOB_ALCHEMIST:
		case JOB_BARD:
		case JOB_DANCER:
			return msg_txt(13 - JOB_CRUSADER + class_);

		case JOB_CRUSADER2:
			return msg_txt(13);

		case JOB_WEDDING:
		case JOB_SUPER_NOVICE:
		case JOB_GUNSLINGER:
		case JOB_NINJA:
		case JOB_XMAS:
			return msg_txt(20 - JOB_WEDDING + class_);

		case JOB_SUMMER:
		case JOB_SUMMER2:
			return msg_txt(71);

		case JOB_HANBOK:
		case JOB_OKTOBERFEST:
			return msg_txt(105 - JOB_HANBOK + class_);

		case JOB_NOVICE_HIGH:
		case JOB_SWORDMAN_HIGH:
		case JOB_MAGE_HIGH:
		case JOB_ARCHER_HIGH:
		case JOB_ACOLYTE_HIGH:
		case JOB_MERCHANT_HIGH:
		case JOB_THIEF_HIGH:
			return msg_txt(25 - JOB_NOVICE_HIGH + class_);

		case JOB_LORD_KNIGHT:
		case JOB_HIGH_PRIEST:
		case JOB_HIGH_WIZARD:
		case JOB_WHITESMITH:
		case JOB_SNIPER:
		case JOB_ASSASSIN_CROSS:
			return msg_txt(32 - JOB_LORD_KNIGHT + class_);

		case JOB_LORD_KNIGHT2:
			return msg_txt(32);

		case JOB_PALADIN:
		case JOB_CHAMPION:
		case JOB_PROFESSOR:
		case JOB_STALKER:
		case JOB_CREATOR:
		case JOB_CLOWN:
		case JOB_GYPSY:
			return msg_txt(38 - JOB_PALADIN + class_);

		case JOB_PALADIN2:
			return msg_txt(38);

		case JOB_BABY:
		case JOB_BABY_SWORDMAN:
		case JOB_BABY_MAGE:
		case JOB_BABY_ARCHER:
		case JOB_BABY_ACOLYTE:
		case JOB_BABY_MERCHANT:
		case JOB_BABY_THIEF:
			return msg_txt(45 - JOB_BABY + class_);

		case JOB_BABY_KNIGHT:
		case JOB_BABY_PRIEST:
		case JOB_BABY_WIZARD:
		case JOB_BABY_BLACKSMITH:
		case JOB_BABY_HUNTER:
		case JOB_BABY_ASSASSIN:
			return msg_txt(52 - JOB_BABY_KNIGHT + class_);

		case JOB_BABY_KNIGHT2:
			return msg_txt(52);

		case JOB_BABY_CRUSADER:
		case JOB_BABY_MONK:
		case JOB_BABY_SAGE:
		case JOB_BABY_ROGUE:
		case JOB_BABY_ALCHEMIST:
		case JOB_BABY_BARD:
		case JOB_BABY_DANCER:
			return msg_txt(58 - JOB_BABY_CRUSADER + class_);

		case JOB_BABY_CRUSADER2:
			return msg_txt(58);

		case JOB_SUPER_BABY:
			return msg_txt(65);

		case JOB_TAEKWON:
			return msg_txt(66);
		case JOB_STAR_GLADIATOR:
		case JOB_STAR_GLADIATOR2:
			return msg_txt(67);
		case JOB_SOUL_LINKER:
			return msg_txt(68);

		case JOB_GANGSI:
		case JOB_DEATH_KNIGHT:
		case JOB_DARK_COLLECTOR:
			return msg_txt(72 - JOB_GANGSI + class_);

		case JOB_RUNE_KNIGHT:
		case JOB_WARLOCK:
		case JOB_RANGER:
		case JOB_ARCH_BISHOP:
		case JOB_MECHANIC:
		case JOB_GUILLOTINE_CROSS:
			return msg_txt(75 - JOB_RUNE_KNIGHT + class_);

		case JOB_RUNE_KNIGHT_T:
		case JOB_WARLOCK_T:
		case JOB_RANGER_T:
		case JOB_ARCH_BISHOP_T:
		case JOB_MECHANIC_T:
		case JOB_GUILLOTINE_CROSS_T:
			return msg_txt(75 - JOB_RUNE_KNIGHT_T + class_);

		case JOB_ROYAL_GUARD:
		case JOB_SORCERER:
		case JOB_MINSTREL:
		case JOB_WANDERER:
		case JOB_SURA:
		case JOB_GENETIC:
		case JOB_SHADOW_CHASER:
			return msg_txt(81 - JOB_ROYAL_GUARD + class_);

		case JOB_ROYAL_GUARD_T:
		case JOB_SORCERER_T:
		case JOB_MINSTREL_T:
		case JOB_WANDERER_T:
		case JOB_SURA_T:
		case JOB_GENETIC_T:
		case JOB_SHADOW_CHASER_T:
			return msg_txt(81 - JOB_ROYAL_GUARD_T + class_);

		case JOB_RUNE_KNIGHT2:
		case JOB_RUNE_KNIGHT_T2:
			return msg_txt(75);

		case JOB_ROYAL_GUARD2:
		case JOB_ROYAL_GUARD_T2:
			return msg_txt(81);

		case JOB_RANGER2:
		case JOB_RANGER_T2:
			return msg_txt(77);

		case JOB_MECHANIC2:
		case JOB_MECHANIC_T2:
			return msg_txt(79);

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
			return msg_txt(88 - JOB_BABY_RUNE + class_);

		case JOB_BABY_RUNE2:
			return msg_txt(88);

		case JOB_BABY_GUARD2:
			return msg_txt(94);

		case JOB_BABY_RANGER2:
			return msg_txt(90);

		case JOB_BABY_MECHANIC2:
			return msg_txt(92);

		case JOB_SUPER_NOVICE_E:
		case JOB_SUPER_BABY_E:
			return msg_txt(101 - JOB_SUPER_NOVICE_E + class_);

		case JOB_KAGEROU:
		case JOB_OBORO:
			return msg_txt(103 - JOB_KAGEROU + class_);

		case JOB_REBELLION:
			return msg_txt(106);

		case JOB_SUMMONER:
			return msg_txt(108);

		case JOB_BABY_SUMMONER:
			return msg_txt(109);

		case JOB_BABY_NINJA:
		case JOB_BABY_KAGEROU:
		case JOB_BABY_OBORO:
		case JOB_BABY_TAEKWON:
		case JOB_BABY_STAR_GLADIATOR:
		case JOB_BABY_SOUL_LINKER:
		case JOB_BABY_GUNSLINGER:
		case JOB_BABY_REBELLION:
			return msg_txt(110 - JOB_BABY_NINJA + class_);

		case JOB_BABY_STAR_GLADIATOR2:
		case JOB_STAR_EMPEROR:
		case JOB_SOUL_REAPER:
		case JOB_BABY_STAR_EMPEROR:
		case JOB_BABY_SOUL_REAPER:
			return msg_txt(114 - JOB_BABY_STAR_GLADIATOR2 + class_);

		case JOB_STAR_EMPEROR2:
			return msg_txt(118);

		case JOB_BABY_STAR_EMPEROR2:
			return msg_txt(120);

		default:
			return msg_txt(199);
	}
}

/**
 * [Dekamaster/Nightroad]
 */
#define GEOIP_MAX_COUNTRIES 255
#define GEOIP_STRUCTURE_INFO_MAX_SIZE 20
#define GEOIP_COUNTRY_BEGIN 16776960

const char *geoip_countryname[GEOIP_MAX_COUNTRIES] = {"Unknown", "Asia/Pacific Region", "Europe", "Andorra", "United Arab Emirates", "Afghanistan", "Antigua and Barbuda", "Anguilla", "Albania", "Armenia", "Netherlands Antilles",
		"Angola", "Antarctica", "Argentina", "American Samoa", "Austria", "Australia", "Aruba", "Azerbaijan", "Bosnia and Herzegovina", "Barbados",
		"Bangladesh", "Belgium", "Burkina Faso", "Bulgaria", "Bahrain", "Burundi", "Benin", "Bermuda", "Brunei Darussalam", "Bolivia",
		"Brazil", "Bahamas", "Bhutan", "Bouvet Island", "Botswana", "Belarus", "Belize", "Canada", "Cocos (Keeling) Islands", "Congo, The Democratic Republic of the",
		"Central African Republic", "Congo", "Switzerland", "Cote D'Ivoire", "Cook Islands", "Chile", "Cameroon", "China", "Colombia", "Costa Rica",
		"Cuba", "Cape Verde", "Christmas Island", "Cyprus", "Czech Republic", "Germany", "Djibouti", "Denmark", "Dominica", "Dominican Republic",
		"Algeria", "Ecuador", "Estonia", "Egypt", "Western Sahara", "Eritrea", "Spain", "Ethiopia", "Finland", "Fiji",
		"Falkland Islands (Malvinas)", "Micronesia, Federated States of", "Faroe Islands", "France", "France, Metropolitan", "Gabon", "United Kingdom", "Grenada", "Georgia", "French Guiana",
		"Ghana", "Gibraltar", "Greenland", "Gambia", "Guinea", "Guadeloupe", "Equatorial Guinea", "Greece", "South Georgia and the South Sandwich Islands", "Guatemala",
		"Guam", "Guinea-Bissau", "Guyana", "Hong Kong", "Heard Island and McDonald Islands", "Honduras", "Croatia", "Haiti", "Hungary", "Indonesia",
		"Ireland", "Israel", "India", "British Indian Ocean Territory", "Iraq", "Iran, Islamic Republic of", "Iceland", "Italy", "Jamaica", "Jordan",
		"Japan", "Kenya", "Kyrgyzstan", "Cambodia", "Kiribati", "Comoros", "Saint Kitts and Nevis", "Korea, Democratic People's Republic of", "Korea, Republic of", "Kuwait",
		"Cayman Islands", "Kazakhstan", "Lao People's Democratic Republic", "Lebanon", "Saint Lucia", "Liechtenstein", "Sri Lanka", "Liberia", "Lesotho", "Lithuania",
		"Luxembourg", "Latvia", "Libyan Arab Jamahiriya", "Morocco", "Monaco", "Moldova, Republic of", "Madagascar", "Marshall Islands", "Macedonia", "Mali",
		"Myanmar", "Mongolia", "Macau", "Northern Mariana Islands", "Martinique", "Mauritania", "Montserrat", "Malta", "Mauritius", "Maldives",
		"Malawi", "Mexico", "Malaysia", "Mozambique", "Namibia", "New Caledonia", "Niger", "Norfolk Island", "Nigeria", "Nicaragua",
		"Netherlands", "Norway", "Nepal", "Nauru", "Niue", "New Zealand", "Oman", "Panama", "Peru", "French Polynesia",
		"Papua New Guinea", "Philippines", "Pakistan", "Poland", "Saint Pierre and Miquelon", "Pitcairn Islands", "Puerto Rico", "Palestinian Territory", "Portugal", "Palau",
		"Paraguay", "Qatar", "Reunion", "Romania", "Russian Federation", "Rwanda", "Saudi Arabia", "Solomon Islands", "Seychelles", "Sudan",
		"Sweden", "Singapore", "Saint Helena", "Slovenia", "Svalbard and Jan Mayen", "Slovakia", "Sierra Leone", "San Marino", "Senegal", "Somalia", "Suriname",
		"Sao Tome and Principe", "El Salvador", "Syrian Arab Republic", "Swaziland", "Turks and Caicos Islands", "Chad", "French Southern Territories", "Togo", "Thailand",
		"Tajikistan", "Tokelau", "Turkmenistan", "Tunisia", "Tonga", "Timor-Leste", "Turkey", "Trinidad and Tobago", "Tuvalu", "Taiwan",
		"Tanzania, United Republic of", "Ukraine", "Uganda", "United States Minor Outlying Islands", "United States", "Uruguay", "Uzbekistan", "Holy See (Vatican City State)", "Saint Vincent and the Grenadines", "Venezuela",
		"Virgin Islands, British", "Virgin Islands, U.S.", "Vietnam", "Vanuatu", "Wallis and Futuna", "Samoa", "Yemen", "Mayotte", "Serbia", "South Africa",
		"Zambia", "Montenegro", "Zimbabwe", "Anonymous Proxy", "Satellite Provider", "Other", "Aland Islands", "Guernsey", "Isle of Man", "Jersey",
		"Saint Barthelemy", "Saint Martin", "Bonaire, Saint Eustatius and Saba", "South Sudan"};

/**
 * GeoIP information
 */
struct s_geoip {
	unsigned char *cache; // GeoIP.dat information see geoip_init()
	bool active;
} geoip;

/* [Dekamaster/Nightroad] */
/* WHY NOT A DBMAP: There are millions of entries in GeoIP and it has its own algorithm to go quickly through them, a DBMap wouldn't be efficient */
const char *geoip_getcountry(uint32 ipnum) {
	int depth;
	unsigned int x;
	unsigned int offset = 0;

	if( geoip.active == false )
		return geoip_countryname[0];

	for( depth = 31; depth >= 0; depth-- ) {
		const unsigned char *buf = geoip.cache + (long)6 * offset;

		if( ipnum & (1 << depth) ) {
			/* Take the right-hand branch */
			x =   (buf[3 * 1 + 0] << (0 * 8))
				+ (buf[3 * 1 + 1] << (1 * 8))
				+ (buf[3 * 1 + 2] << (2 * 8));
		} else {
			/* Take the left-hand branch */
			x =   (buf[3 * 0 + 0] << (0 * 8))
				+ (buf[3 * 0 + 1] << (1 * 8))
				+ (buf[3 * 0 + 2] << (2 * 8));
		}
		if( x >= GEOIP_COUNTRY_BEGIN ) {
			x = x - GEOIP_COUNTRY_BEGIN;
			if( x > GEOIP_MAX_COUNTRIES )
				return geoip_countryname[0];
			return geoip_countryname[x];
		}
		offset = x;
	}

	ShowError("geoip_getcountry(): Error traversing database for ipnum %d\n", ipnum);
	ShowWarning("geoip_getcountry(): Possible database corruption!\n");

	return geoip_countryname[0];
}

/**
 * Disables GeoIP
 * frees geoip.cache
 */
void geoip_final(bool shutdown) {
	if( geoip.cache ) {
		aFree(geoip.cache);
		geoip.cache = NULL;
	}

	if( geoip.active ) {
		if( !shutdown )
			ShowStatus("GeoIP "CL_RED"disabled"CL_RESET".\n");
		geoip.active = false;
	}
}

/**
 * Reads GeoIP database and stores it into memory
 * geoip.cache should be freed after use!
 * http://dev.maxmind.com/geoip/legacy/geolite/
 */
void geoip_init(void) {
	int fno;
	char db_type = 1;
	struct stat bufa;
	FILE *db;

	geoip.active = true;

	db = fopen("./db/GeoIP.dat","rb");
	if( db == NULL ) {
		ShowError("geoip_readdb: Error reading GeoIP.dat!\n");
		geoip_final(false);
		return;
	}

	fno = fileno(db);
	if( fstat(fno, &bufa) < 0 ) {
		ShowError("geoip_readdb: Error stating GeoIP.dat! Error %d\n", errno);
		geoip_final(false);
		return;
	}

	geoip.cache = aMalloc((sizeof(geoip.cache) * bufa.st_size));
	if( fread(geoip.cache, sizeof(unsigned char), bufa.st_size, db) != bufa.st_size ) {
		ShowError("geoip_cache: Couldn't read all elements!\n");
		fclose(db);
		geoip_final(false);
		return;
	}

	// Search database type
	if( fseek(db, -3l, SEEK_END) != 0 )
		db_type = 0;
	else {
		int i;
		unsigned char delim[3];

		for( i = 0; i < GEOIP_STRUCTURE_INFO_MAX_SIZE; i++ ) {
			if( fread(delim, sizeof(delim[0]), 3, db) != 3 ) {
				db_type = 0;
				break;
			}
			if( delim[0] == 255 && delim[1] == 255 && delim[2] == 255 ) {
				if( fread(&db_type, sizeof(db_type), 1, db) != 1)
					db_type = 0;
				break;
			}
			if( fseek(db, -4l, SEEK_CUR) != 0 ) {
				db_type = 0;
				break;
			}
		}
	}

	fclose(db);

	if( db_type != 1 ) {
		if( db_type )
			ShowError("geoip_init(): Database type is not supported %d!\n", db_type);
		else
			ShowError("geoip_init(): GeoIP is corrupted!\n");

		geoip_final(false);
		return;
	}

	ShowStatus("Finished Reading "CL_GREEN"GeoIP"CL_RESET" Database.\n");
}

/* Sends a mesasge to map server (fd) to a user (u_fd) although we use fd we keep aid for safe-check */
/* Extremely handy I believe it will serve other uses in the near future */
void inter_to_fd(int fd, int u_fd, int aid, char *msg, ...) {
	char msg_out[512];
	va_list ap;
	int len = 1; /* Yes we start at 1 */

	va_start(ap, msg);
		len += vsnprintf(msg_out, 512, msg, ap);
	va_end(ap);

	WFIFOHEAD(fd,12 + len);

	WFIFOW(fd,0) = 0x3807;
	WFIFOW(fd,2) = 12 + (unsigned short)len;
	WFIFOL(fd,4) = u_fd;
	WFIFOL(fd,8) = aid;
	safestrncpy((char *)WFIFOP(fd,12), msg_out, len);

	WFIFOSET(fd,12 + len);

	return;
}

/* [Dekamaster/Nightroad] */
void mapif_parse_accinfo(int fd) {
	int u_fd = RFIFOL(fd,2), aid = RFIFOL(fd,6), castergroup = RFIFOL(fd,10);
	char query[NAME_LENGTH], query_esq[NAME_LENGTH * 2 + 1];
	uint32 account_id;
	char *data;

	safestrncpy(query, (char *)RFIFOP(fd,14), NAME_LENGTH);

	Sql_EscapeString(sql_handle, query_esq, query);

	account_id = atoi(query);

	if( account_id < START_ACCOUNT_NUM ) {	// Is string
		if( SQL_ERROR == Sql_Query(sql_handle, "SELECT `account_id`,`name`,`class`,`base_level`,`job_level`,`online` FROM `%s` WHERE `name` LIKE '%s' LIMIT 10", char_db, query_esq)
			|| Sql_NumRows(sql_handle) == 0 ) {
			if( Sql_NumRows(sql_handle) == 0 )
				inter_to_fd(fd, u_fd, aid, "No matches were found for your criteria, '%s'", query);
			else {
				Sql_ShowDebug(sql_handle);
				inter_to_fd(fd, u_fd, aid, "An error occured, bother your admin about it.");
			}
			Sql_FreeResult(sql_handle);
			return;
		} else {
			if( Sql_NumRows(sql_handle) == 1 ) { // We found a perfect match
				Sql_NextRow(sql_handle);
				Sql_GetData(sql_handle, 0, &data, NULL); account_id = atoi(data);
				Sql_FreeResult(sql_handle);
			} else { // More than one, listing. [Dekamaster/Nightroad]
				inter_to_fd(fd, u_fd, aid, "Your query returned the following %d results, please be more specific...",(int)Sql_NumRows(sql_handle));
				while( SQL_SUCCESS == Sql_NextRow(sql_handle) ) {
					int class_;
					short base_level, job_level, online;
					char name[NAME_LENGTH];

					Sql_GetData(sql_handle, 0, &data, NULL); account_id = atoi(data);
					Sql_GetData(sql_handle, 1, &data, NULL); safestrncpy(name, data, sizeof(name));
					Sql_GetData(sql_handle, 2, &data, NULL); class_ = atoi(data);
					Sql_GetData(sql_handle, 3, &data, NULL); base_level = atoi(data);
					Sql_GetData(sql_handle, 4, &data, NULL); job_level = atoi(data);
					Sql_GetData(sql_handle, 5, &data, NULL); online = atoi(data);

					inter_to_fd(fd, u_fd, aid, "[AID: %d] %s | %s | Level: %d/%d | %s", account_id, name, job_name(class_), base_level, job_level, online?"Online":"Offline");
				}
				Sql_FreeResult(sql_handle);
				return;
			}
		}
	}

	/* It will only get here if we have a single match */
	/* And we will send packet with account id to login server asking for account info */
	if( account_id )
		mapif_on_parse_accinfo(account_id, u_fd, aid, castergroup, fd);

	return;
}

void mapif_parse_accinfo2(bool success, int map_fd, int u_fd, int u_aid, int account_id, const char *userid, const char *user_pass, const char *email, const char *last_ip, const char *lastlogin, const char *pin_code, const char *birthdate, int group_id, int logincount, int state) {
	if( map_fd <= 0 || !session_isActive(map_fd) )
		return; // Check if we have a valid fd

	if( !success ) {
		inter_to_fd(map_fd, u_fd, u_aid, "No account with ID '%d' was found.", account_id);
		return;
	}

	inter_to_fd(map_fd, u_fd, u_aid, "-- Account %d --", account_id);
	inter_to_fd(map_fd, u_fd, u_aid, "User: %s | GM Group: %d | State: %d", userid, group_id, state);

	/* Password is only received if your gm level is greater than the one you're searching for */
	if( user_pass && *user_pass != '\0' ) {
		if( pin_code && *pin_code != '\0' )
			inter_to_fd(map_fd, u_fd, u_aid, "Password: %s (PIN:%s)", user_pass, pin_code);
		else
			inter_to_fd(map_fd, u_fd, u_aid, "Password: %s", user_pass );
	}

	inter_to_fd(map_fd, u_fd, u_aid, "Account e-mail: %s | Birthdate: %s", email, birthdate);
	inter_to_fd(map_fd, u_fd, u_aid, "Last IP: %s (%s)", last_ip, geoip_getcountry(str2ip(last_ip)));
	inter_to_fd(map_fd, u_fd, u_aid, "This user has logged %d times, the last time were at %s", logincount, lastlogin);
	inter_to_fd(map_fd, u_fd, u_aid, "-- Character Details --");

	if( SQL_ERROR == Sql_Query(sql_handle, "SELECT `char_id`, `name`, `char_num`, `class`, `base_level`, `job_level`, `online` FROM `%s` WHERE `account_id` = '%d' ORDER BY `char_num` LIMIT %d",
		char_db, account_id, MAX_CHARS) || Sql_NumRows(sql_handle) == 0 )
	{
		if( Sql_NumRows(sql_handle) == 0 )
			inter_to_fd(map_fd, u_fd, u_aid, "This account doesn't have characters.");
		else {
			inter_to_fd(map_fd, u_fd, u_aid, "An error occured, bother your admin about it.");
			Sql_ShowDebug(sql_handle);
		}
	} else {
		while( SQL_SUCCESS == Sql_NextRow(sql_handle) ) {
			char *data;
			uint32 char_id, class_;
			short char_num, base_level, job_level, online;
			char name[NAME_LENGTH];

			Sql_GetData(sql_handle, 0, &data, NULL); char_id = atoi(data);
			Sql_GetData(sql_handle, 1, &data, NULL); safestrncpy(name, data, sizeof(name));
			Sql_GetData(sql_handle, 2, &data, NULL); char_num = atoi(data);
			Sql_GetData(sql_handle, 3, &data, NULL); class_ = atoi(data);
			Sql_GetData(sql_handle, 4, &data, NULL); base_level = atoi(data);
			Sql_GetData(sql_handle, 5, &data, NULL); job_level = atoi(data);
			Sql_GetData(sql_handle, 6, &data, NULL); online = atoi(data);

			inter_to_fd(map_fd, u_fd, u_aid, "[Slot/CID: %d/%d] %s | %s | Level: %d/%d | %s", char_num, char_id, name, job_name(class_), base_level, job_level, online ? "On" : "Off");
		}
	}
	Sql_FreeResult(sql_handle);

	return;
}

//--------------------------------------------------------
// Save registry to sql
int inter_accreg_tosql(int account_id, int char_id, struct accreg* reg, int type)
{
	StringBuf buf;
	int i;

	if( account_id <= 0 )
		return 0;
	reg->account_id = account_id;
	reg->char_id = char_id;

	//`global_reg_value` (`type`, `account_id`, `char_id`, `str`, `value`)
	switch( type ) {
		case 3: // Char Reg
			if( SQL_ERROR == Sql_Query(sql_handle, "DELETE FROM `%s` WHERE `type`=3 AND `char_id`='%d'", reg_db, char_id) )
				Sql_ShowDebug(sql_handle);
			account_id = 0;
			break;
		case 2: // Account Reg
			if( SQL_ERROR == Sql_Query(sql_handle, "DELETE FROM `%s` WHERE `type`=2 AND `account_id`='%d'", reg_db, account_id) )
				Sql_ShowDebug(sql_handle);
			char_id = 0;
			break;
		case 1: // Account2 Reg
			ShowError("inter_accreg_tosql: Char server shouldn't handle type 1 registry values (##). That is the login server's work!\n");
			return 0;
		default:
			ShowError("inter_accreg_tosql: Invalid type %d\n", type);
			return 0;
	}

	if( reg->reg_num <= 0 )
		return 0;

	StringBuf_Init(&buf);
	StringBuf_Printf(&buf, "INSERT INTO `%s` (`type`,`account_id`,`char_id`,`str`,`value`) VALUES ", reg_db);

	for( i = 0; i < reg->reg_num; ++i ) {
		struct global_reg *r = &reg->reg[i];

		if( r->str[0] != '\0' && r->value[0] != '\0' ) {
			char str[32];
			char val[256];

			if( i > 0 )
				StringBuf_AppendStr(&buf, ",");

			Sql_EscapeString(sql_handle, str, r->str);
			Sql_EscapeString(sql_handle, val, r->value);

			StringBuf_Printf(&buf, "('%d','%d','%d','%s','%s')", type, account_id, char_id, str, val);
		}
	}

	if( SQL_ERROR == Sql_QueryStr(sql_handle, StringBuf_Value(&buf)) )
		Sql_ShowDebug(sql_handle);

	StringBuf_Destroy(&buf);

	return 1;
}

// Load account_reg from sql (type=2)
int inter_accreg_fromsql(int account_id,int char_id, struct accreg *reg, int type)
{
	char *data;
	size_t len;
	int i;

	if( reg == NULL )
		return 0;

	memset(reg, 0, sizeof(struct accreg));
	reg->account_id = account_id;
	reg->char_id = char_id;

	//`global_reg_value` (`type`, `account_id`, `char_id`, `str`, `value`)
	switch( type ) {
		case 3: // Char reg
			if( SQL_ERROR == Sql_Query(sql_handle, "SELECT `str`, `value` FROM `%s` WHERE `type`=3 AND `char_id`='%d'", reg_db, char_id) )
				Sql_ShowDebug(sql_handle);
			break;
		case 2: // Account reg
			if( SQL_ERROR == Sql_Query(sql_handle, "SELECT `str`, `value` FROM `%s` WHERE `type`=2 AND `account_id`='%d'", reg_db, account_id) )
				Sql_ShowDebug(sql_handle);
			break;
		case 1: // Account2 reg
			ShowError("inter_accreg_fromsql: Char server shouldn't handle type 1 registry values (##). That is the login server's work!\n");
			return 0;
		default:
			ShowError("inter_accreg_fromsql: Invalid type %d\n", type);
			return 0;
	}
	for( i = 0; i < MAX_REG_NUM && SQL_SUCCESS == Sql_NextRow(sql_handle); ++i ) {
		struct global_reg *r = &reg->reg[i];

		// Str
		Sql_GetData(sql_handle, 0, &data, &len);
		memcpy(r->str, data, zmin(len, sizeof(r->str)));
		// Value
		Sql_GetData(sql_handle, 1, &data, &len);
		memcpy(r->value, data, zmin(len, sizeof(r->value)));
	}
	reg->reg_num = i;
	Sql_FreeResult(sql_handle);
	return 1;
}

// Initialize
int inter_accreg_sql_init(void)
{
	CREATE(accreg_pt, struct accreg, 1);
	return 0;

}

/*==========================================
 * read config file
 *------------------------------------------*/
static int inter_config_read(const char *cfgName)
{
	char line[1024];
	FILE *fp;

	fp = fopen(cfgName, "r");
	if(fp == NULL) {
		ShowError("File not found: %s\n", cfgName);
		return 1;
	}

	while(fgets(line, sizeof(line), fp)) {
		char w1[24], w2[1024];

		if(line[0] == '/' && line[1] == '/')
			continue;

		if(sscanf(line, "%23[^:]: %1023[^\r\n]", w1, w2) != 2)
			continue;

		if(!strcmpi(w1, "char_server_ip"))
			safestrncpy(char_server_ip, w2 ,sizeof(char_server_ip));
		else if(!strcmpi(w1, "char_server_port"))
			char_server_port = atoi(w2);
		else if(!strcmpi(w1, "char_server_id"))
			safestrncpy(char_server_id, w2, sizeof(char_server_id));
		else if(!strcmpi(w1, "char_server_pw"))
			safestrncpy(char_server_pw, w2, sizeof(char_server_pw));
		else if(!strcmpi(w1, "char_server_db"))
			safestrncpy(char_server_db, w2, sizeof(char_server_db));
		else if(!strcmpi(w1, "default_codepage"))
			safestrncpy(default_codepage, w2, sizeof(default_codepage));
		else if(!strcmpi(w1, "party_share_level"))
			party_share_level = (unsigned int)atof(w2);
		else if(!strcmpi(w1, "log_inter"))
			log_inter = atoi(w2);
		else if(!strcmpi(w1, "inter_server_conf"))
			safestrncpy(interserv_config.cfgFile, w2, sizeof(interserv_config.cfgFile));
		else if(!strcmpi(w1, "import"))
			inter_config_read(w2);
	}
	fclose(fp);

	ShowInfo ("Done reading %s.\n", cfgName);

	return 0;
}

// Save interlog into sql
int inter_log(char *fmt, ...)
{
	char str[255];
	char esc_str[sizeof(str) * 2 + 1]; // Escaped str
	va_list ap;

	va_start(ap,fmt);
	vsnprintf(str, sizeof(str), fmt, ap);
	va_end(ap);

	Sql_EscapeStringLen(sql_handle, esc_str, str, strnlen(str, sizeof(str)));
	if( SQL_ERROR == Sql_Query(sql_handle, "INSERT INTO `%s` (`time`, `log`) VALUES (NOW(), '%s')", interlog_db, esc_str) )
		Sql_ShowDebug(sql_handle);

	return 0;
}

/**
 * Read inter config file
 */
static void inter_config_readConf(void)
{
	int count = 0;
	struct config_setting_t *config = NULL;

	if( conf_read_file(&interserv_config.cfg, interserv_config.cfgFile) )
		return;

	// Read storages
	config = config_lookup(&interserv_config.cfg, "storages");
	if( config && (count = config_setting_length(config)) ) {
		int i;

		for( i = 0; i < count; i++ ) {
			int id, max_num;
			const char *name, *tablename;
			struct s_storage_table table;
			struct config_setting_t *entry = config_setting_get_elem(config, i);

			if( !config_setting_lookup_int(entry, "id", &id) ) {
				ShowConfigWarning(entry, "inter_config_readConf: Cannot find storage \"id\" in member %d", i);
				continue;
			}

			if( !config_setting_lookup_string(entry, "name", &name) ) {
				ShowConfigWarning(entry, "inter_config_readConf: Cannot find storage \"name\" in member %d", i);
				continue;
			}

			if( !config_setting_lookup_string(entry, "table", &tablename) ) {
				ShowConfigWarning(entry, "inter_config_readConf: Cannot find storage \"table\" in member %d", i);
				continue;
			}

			if( !config_setting_lookup_int(entry, "max", &max_num) )
				max_num = MAX_STORAGE;
			else if( max_num > MAX_STORAGE ) {
				ShowConfigWarning(entry, "Storage \"%s\" has \"max\" %d, max is MAX_STORAGE (%d)!\n", name, max_num, MAX_STORAGE);
				max_num = MAX_STORAGE;
			}

			memset(&table, 0, sizeof(struct s_storage_table));

			RECREATE(interserv_config.storages, struct s_storage_table, interserv_config.storage_count + 1);
			interserv_config.storages[interserv_config.storage_count].id = id;
			safestrncpy(interserv_config.storages[interserv_config.storage_count].name, name, NAME_LENGTH);
			safestrncpy(interserv_config.storages[interserv_config.storage_count].table, tablename, DB_NAME_LEN);
			interserv_config.storages[interserv_config.storage_count].max_num = max_num;
			interserv_config.storage_count++;
		}
	}

	ShowStatus("Done reading '"CL_WHITE"%d"CL_RESET"' storage information in '"CL_WHITE"%s"CL_RESET"'\n", interserv_config.storage_count, interserv_config.cfgFile);
}

void inter_config_finalConf(void)
{

	if( interserv_config.storages )
		aFree(interserv_config.storages);
	interserv_config.storages = NULL;
	interserv_config.storage_count = 0;

	config_destroy(&interserv_config.cfg);
}

static void inter_config_defaults(void)
{
	interserv_config.storage_count = 0;
	interserv_config.storages = NULL;

	safestrncpy(interserv_config.cfgFile, "conf/inter_server.conf", sizeof(interserv_config.cfgFile));
}

// Initialize
int inter_init_sql(const char *file)
{
	//int i;

	inter_config_defaults();
	inter_config_read(file);

	//DB connection initialized
	sql_handle = Sql_Malloc();
	ShowInfo("Connect Character DB server.... (Character Server)\n");
	if( SQL_ERROR == Sql_Connect(sql_handle, char_server_id, char_server_pw, char_server_ip, (uint16)char_server_port, char_server_db) ) {
		ShowError("Couldn't connect with username = '%s', password = '%s', host = '%s', port = '%d', database = '%s'\n",
			char_server_id, char_server_pw, char_server_ip, char_server_port, char_server_db);
		Sql_ShowDebug(sql_handle);
		Sql_Free(sql_handle);
		exit(EXIT_FAILURE);
	}

	if( *default_codepage ) {
		if( SQL_ERROR == Sql_SetEncoding(sql_handle, default_codepage) )
			Sql_ShowDebug(sql_handle);
	}

	wis_db = idb_alloc(DB_OPT_RELEASE_DATA);
	inter_config_readConf();
	inter_guild_sql_init();
	inter_storage_sql_init();
	inter_party_sql_init();
	inter_pet_sql_init();
	inter_homunculus_sql_init();
	inter_mercenary_sql_init();
	inter_elemental_sql_init();
	inter_accreg_sql_init();
	inter_mail_sql_init();
	inter_auction_sql_init();
	inter_clan_sql_init();

	geoip_init();
	return 0;
}

// Finalize
void inter_final(void)
{
	wis_db->destroy(wis_db, NULL);

	inter_config_finalConf();
	inter_guild_sql_final();
	inter_storage_sql_final();
	inter_party_sql_final();
	inter_pet_sql_final();
	inter_homunculus_sql_final();
	inter_mercenary_sql_final();
	inter_elemental_sql_final();
	inter_mail_sql_final();
	inter_auction_sql_final();
	inter_clan_sql_final();

	if( accreg_pt )
		aFree(accreg_pt);

	geoip_final(true);

	return;
}

/**
 * IZ 0x388c <len>.W { <storage_table>.? }*?
 * Sends storage information to map-server
 * @param fd
 */
void inter_Storage_sendInfo(int fd)
{
	int size = sizeof(struct s_storage_table), len = 4 + interserv_config.storage_count * size, i = 0;
	// Send storage table information
	WFIFOHEAD(fd,len);
	WFIFOW(fd,0) = 0x388c;
	WFIFOW(fd,2) = len;
	for( i = 0; i < interserv_config.storage_count; i++ ) {
		if( !&interserv_config.storages[i] || !interserv_config.storages[i].name )
			continue;
		memcpy(WFIFOP(fd, 4 + size * i), &interserv_config.storages[i], size);
	}
	WFIFOSET(fd,len);
}

int inter_mapif_init(int fd)
{
	inter_Storage_sendInfo(fd);
	return 0;
}


//--------------------------------------------------------

// Broadcast sending
int mapif_broadcast(unsigned char *mes, int len, unsigned long fontColor, short fontType, short fontSize, short fontAlign, short fontY, int sfd)
{
	unsigned char *buf = (unsigned char *)aMalloc((len) * sizeof(unsigned char));

	if (buf == NULL) return 1;

	WBUFW(buf,0) = 0x3800;
	WBUFW(buf,2) = len;
	WBUFL(buf,4) = fontColor;
	WBUFW(buf,8) = fontType;
	WBUFW(buf,10) = fontSize;
	WBUFW(buf,12) = fontAlign;
	WBUFW(buf,14) = fontY;
	memcpy(WBUFP(buf,16), mes, len - 16);
	mapif_sendallwos(sfd, buf, len);

	aFree(buf);
	return 0;
}

// Wis sending
int mapif_wis_message(struct WisData *wd)
{
	unsigned char buf[2048];

	if (wd->len >= sizeof(wd->msg) - 1) //Force it to fit to avoid crashes [Skotlex]
		wd->len = sizeof(wd->msg) - 1;

	WBUFW(buf,0) = 0x3801;
	WBUFW(buf,2) = 56 + wd->len;
	WBUFL(buf,4) = wd->id;
	memcpy(WBUFP(buf, 8), wd->src, NAME_LENGTH);
	memcpy(WBUFP(buf,32), wd->dst, NAME_LENGTH);
	memcpy(WBUFP(buf,56), wd->msg, wd->len);
	wd->count = mapif_sendall(buf,WBUFW(buf,2));

	return 0;
}

// Wis sending result
int mapif_wis_end(struct WisData *wd, int flag)
{
	unsigned char buf[27];

	WBUFW(buf, 0)=0x3802;
	memcpy(WBUFP(buf, 2),wd->src,24);
	WBUFB(buf,26)=flag;
	mapif_send(wd->fd,buf,27);
	return 0;
}

// Account registry transfer to map-server
static void mapif_account_reg(int fd, unsigned char *src)
{
	WBUFW(src,0)=0x3804; //NOTE: writing to RFIFO
	mapif_sendallwos(fd, src, WBUFW(src,2));
}

// Send the requested account_reg
int mapif_account_reg_reply(int fd,int account_id,int char_id, int type)
{
	struct accreg *reg=accreg_pt;
	WFIFOHEAD(fd,13 + 5000);
	inter_accreg_fromsql(account_id,char_id,reg,type);

	WFIFOW(fd,0)=0x3804;
	WFIFOL(fd,4)=account_id;
	WFIFOL(fd,8)=char_id;
	WFIFOB(fd,12)=type;
	if(reg->reg_num==0){
		WFIFOW(fd,2)=13;
	}else{
		int i,p;
		for (p=13,i = 0; i < reg->reg_num && p < 5000; i++) {
			p+= sprintf((char *)WFIFOP(fd,p), "%s", reg->reg[i].str)+1; //We add 1 to consider the '\0' in place.
			p+= sprintf((char *)WFIFOP(fd,p), "%s", reg->reg[i].value)+1;
		}
		WFIFOW(fd,2)=p;
		if (p>= 5000)
			ShowWarning("Too many acc regs for %d:%d, not all values were loaded.\n", account_id, char_id);
	}
	WFIFOSET(fd,WFIFOW(fd,2));
	return 0;
}

//Request to kick char from a certain map server. [Skotlex]
int mapif_disconnectplayer(int fd, int account_id, int char_id, int reason)
{
	if (fd >= 0) {
		WFIFOHEAD(fd,7);
		WFIFOW(fd,0) = 0x2b1f;
		WFIFOL(fd,2) = account_id;
		WFIFOB(fd,6) = reason;
		WFIFOSET(fd,7);
		return 0;
	}
	return -1;
}

//--------------------------------------------------------

/**
 * Existence check of WISP data
 * @see DBApply
 */
int check_ttl_wisdata_sub(DBKey key, DBData *data, va_list ap)
{
	unsigned long tick;
	struct WisData *wd = db_data2ptr(data);
	tick = va_arg(ap, unsigned long);

	if (DIFF_TICK(tick, wd->tick) > WISDATA_TTL && wis_delnum < WISDELLIST_MAX)
		wis_dellist[wis_delnum++] = wd->id;

	return 0;
}

int check_ttl_wisdata(void)
{
	unsigned long tick = gettick();
	int i;

	do {
		wis_delnum = 0;
		wis_db->foreach(wis_db, check_ttl_wisdata_sub, tick);
		for(i = 0; i < wis_delnum; i++) {
			struct WisData *wd = (struct WisData*)idb_get(wis_db, wis_dellist[i]);
			ShowWarning("inter: wis data id=%d time out : from %s to %s\n", wd->id, wd->src, wd->dst);
			// removed. not send information after a timeout. Just no answer for the player
			//mapif_wis_end(wd, 1); // flag: 0: success to send wisper, 1: target character is not loged in?, 2: ignored by target
			idb_remove(wis_db, wd->id);
		}
	} while(wis_delnum >= WISDELLIST_MAX);

	return 0;
}

//--------------------------------------------------------

// Broadcast sending
int mapif_parse_broadcast(int fd)
{
	mapif_broadcast(RFIFOP(fd,16), RFIFOW(fd,2), RFIFOL(fd,4), RFIFOW(fd,8), RFIFOW(fd,10), RFIFOW(fd,12), RFIFOW(fd,14), fd);
	return 0;
}

/**
 * Parse received item broadcast and sends it to all connected map-serves
 * ZI 3009 <cmd>.W <len>.W <nameid>.W <source>.W <type>.B <name>.24B <srcname>.24B
 * IZ 3809 <cmd>.W <len>.W <nameid>.W <source>.W <type>.B <name>.24B <srcname>.24B
 * @param fd
 * @return
 */
int mapif_parse_broadcast_item(int fd)
{
	unsigned char buf[9 + NAME_LENGTH * 2];

	memcpy(WBUFP(buf,0), RFIFOP(fd,0), RFIFOW(fd,2));
	WBUFW(buf,0) = 0x3809;
	mapif_sendallwos(fd, buf, RFIFOW(fd,2));
	return 0;
}

// Wisp/page request to send
int mapif_parse_WisRequest(int fd)
{
	struct WisData* wd;
	char name[NAME_LENGTH];
	char esc_name[NAME_LENGTH * 2 + 1]; // Escaped name
	char *data;
	size_t len;

	if (fd <= 0)
		return 0; // Check if we have a valid fd

	if (RFIFOW(fd,2) - 52 >= sizeof(wd->msg)) {
		ShowWarning("inter: Wis message size too long.\n");
		return 0;
	} else if (RFIFOW(fd,2) - 52 <= 0) { // Normaly, impossible, but who knows
		ShowError("inter: Wis message doesn't exist.\n");
		return 0;
	}

	safestrncpy(name, (char *)RFIFOP(fd,28), NAME_LENGTH); // Received name may be too large and not contain \0! [Skotlex]

	Sql_EscapeStringLen(sql_handle, esc_name, name, strnlen(name, NAME_LENGTH));
	if (SQL_ERROR == Sql_Query(sql_handle, "SELECT `name` FROM `%s` WHERE `name`='%s'", char_db, esc_name))
		Sql_ShowDebug(sql_handle);

	// Search if character exists before to ask all map-servers
	if (SQL_SUCCESS != Sql_NextRow(sql_handle)) {
		unsigned char buf[27];

		WBUFW(buf,0) = 0x3802;
		memcpy(WBUFP(buf,2), RFIFOP(fd,4), NAME_LENGTH);
		WBUFB(buf,26) = 1; // Flag: 0: success to send wisper, 1: target character is not loged in?, 2: ignored by target
		mapif_send(fd, buf, 27);
	} else { // Character exists. So, ask all map-servers to be sure of the correct name, rewrite it
		Sql_GetData(sql_handle, 0, &data, &len);
		memset(name, 0, NAME_LENGTH);
		memcpy(name, data, zmin(len, NAME_LENGTH));
		// If source is destination, don't ask other servers.
		if (strncmp((const char *)RFIFOP(fd,4), name, NAME_LENGTH) == 0) {
			uint8 buf[27];

			WBUFW(buf,0) = 0x3802;
			memcpy(WBUFP(buf,2), RFIFOP(fd,4), NAME_LENGTH);
			WBUFB(buf,26) = 1; // Flag: 0: success to send wisper, 1: target character is not loged in?, 2: ignored by target
			mapif_send(fd, buf, 27);
		} else {
			static int wisid = 0;

			CREATE(wd, struct WisData, 1);

			// Whether the failure of previous wisp/page transmission (timeout)
			check_ttl_wisdata();

			wd->id = ++wisid;
			wd->fd = fd;
			wd->len = RFIFOW(fd,2) - 52;
			memcpy(wd->src, RFIFOP(fd,4), NAME_LENGTH);
			memcpy(wd->dst, RFIFOP(fd,28), NAME_LENGTH);
			memcpy(wd->msg, RFIFOP(fd,52), wd->len);
			wd->tick = gettick();
			idb_put(wis_db, wd->id, wd);
			mapif_wis_message(wd);
		}
	}

	Sql_FreeResult(sql_handle);
	return 0;
}


// Wisp/page transmission result
int mapif_parse_WisReply(int fd)
{
	int id, flag;
	struct WisData *wd;

	id = RFIFOL(fd,2);
	flag = RFIFOB(fd,6);
	wd = (struct WisData*)idb_get(wis_db, id);
	if (wd == NULL)
		return 0;	// This wisp was probably suppress before, because it was timeout of because of target was found on another map-server

	if ((--wd->count) <= 0 || flag != 1) {
		mapif_wis_end(wd, flag); // Flag: 0: success to send wisper, 1: target character is not loged in?, 2: ignored by target
		idb_remove(wis_db, id);
	}

	return 0;
}

// Received wisp message from map-server for ALL gm (just copy the message and resends it to ALL map-servers)
int mapif_parse_WisToGM(int fd)
{
	unsigned char buf[2048]; // 0x3003/0x3803 <packet_len>.w <wispname>.24B <permission>.L <message>.?B

	memcpy(WBUFP(buf,0), RFIFOP(fd,0), RFIFOW(fd,2)); // Message contains the NUL terminator (see intif_wis_message_to_gm())
	WBUFW(buf,0) = 0x3803;
	mapif_sendall(buf, RFIFOW(fd,2));

	return 0;
}

// Save account_reg into sql (type=2)
int mapif_parse_Registry(int fd)
{
	int j, p, len, max;
	struct accreg *reg = accreg_pt;

	memset(accreg_pt,0,sizeof(struct accreg));
	switch (RFIFOB(fd,12)) {
		case 3: //Character registry
			max = GLOBAL_REG_NUM;
			break;
		case 2: //Account Registry
			max = ACCOUNT_REG_NUM;
			break;
		case 1: //Account2 registry, must be sent over to login server.
			return save_accreg2(RFIFOP(fd,4),RFIFOW(fd,2) - 4);
		default:
			return 1;
	}
	for (j = 0, p = 13; j < max && p < RFIFOW(fd,2); j++) {
		sscanf((char *)RFIFOP(fd,p),"%31c%n",reg->reg[j].str,&len);
		reg->reg[j].str[len] = '\0';
		p += len + 1; //+1 to skip the '\0' between strings.
		sscanf((char *)RFIFOP(fd,p),"%255c%n",reg->reg[j].value,&len);
		reg->reg[j].value[len] = '\0';
		p += len + 1;
	}
	reg->reg_num = j;

	inter_accreg_tosql(RFIFOL(fd,4),RFIFOL(fd,8),reg,RFIFOB(fd,12));
	mapif_account_reg(fd,RFIFOP(fd,0));	//Send updated accounts to other map servers.
	return 0;
}

// Request the value of all registries.
int mapif_parse_RegistryRequest(int fd)
{
	//Load Char Registry
	if (RFIFOB(fd,12))
		mapif_account_reg_reply(fd,RFIFOL(fd,2),RFIFOL(fd,6),3);
	//Load Account Registry
	if (RFIFOB(fd,11))
		mapif_account_reg_reply(fd,RFIFOL(fd,2),RFIFOL(fd,6),2);
	//Ask Login Server for Account2 values.
	if (RFIFOB(fd,10))
		request_accreg2(RFIFOL(fd,2),RFIFOL(fd,6));
	return 1;
}

static void mapif_namechange_ack(int fd, int account_id, int char_id, int type, int flag, char *name)
{
	WFIFOHEAD(fd,NAME_LENGTH+13);
	WFIFOW(fd, 0) = 0x3806;
	WFIFOL(fd, 2) = account_id;
	WFIFOL(fd, 6) = char_id;
	WFIFOB(fd,10) = type;
	WFIFOB(fd,11) = flag;
	memcpy(WFIFOP(fd, 12), name, NAME_LENGTH);
	WFIFOSET(fd,NAME_LENGTH+13);
}

int mapif_parse_NameChangeRequest(int fd)
{
	int account_id, char_id, type;
	char *name;
	int i;

	account_id = RFIFOL(fd,2);
	char_id = RFIFOL(fd,6);
	type = RFIFOB(fd,10);
	name = (char *)RFIFOP(fd,11);

	// Check Authorised letters/symbols in the name
	if (char_name_option == 1) { // only letters/symbols in char_name_letters are authorised
		for (i = 0; i < NAME_LENGTH && name[i]; i++)
		if (strchr(char_name_letters, name[i]) == NULL) {
			mapif_namechange_ack(fd, account_id, char_id, type, 0, name);
			return 0;
		}
	} else if (char_name_option == 2) { // letters/symbols in char_name_letters are forbidden
		for (i = 0; i < NAME_LENGTH && name[i]; i++)
		if (strchr(char_name_letters, name[i]) != NULL) {
			mapif_namechange_ack(fd, account_id, char_id, type, 0, name);
			return 0;
		}
	}
	//@TODO: type holds the type of object to rename.
	//If it were a player, it needs to have the guild information and db information
	//updated here, because changing it on the map won't make it be saved [Skotlex]

	//name allowed.
	mapif_namechange_ack(fd, account_id, char_id, type, 1, name);
	return 0;
}

//--------------------------------------------------------

/// Returns the length of the next complete packet to process,
/// or 0 if no complete packet exists in the queue.
///
/// @param length The minimum allowed length, or -1 for dynamic lookup
int inter_check_length(int fd, int length)
{
	if( length == -1 )
	{// variable-length packet
		if( RFIFOREST(fd) < 4 )
			return 0;
		length = RFIFOW(fd,2);
	}

	if( (int)RFIFOREST(fd) < length )
		return 0;

	return length;
}

int inter_parse_frommap(int fd)
{
	int cmd;
	int len = 0;
	cmd = RFIFOW(fd,0);
	// Check is valid packet entry
	if(cmd < 0x3000 || cmd >= 0x3000 + ARRAYLENGTH(inter_recv_packet_length) || inter_recv_packet_length[cmd - 0x3000] == 0)
		return 0;

	// Check packet length
	if((len = inter_check_length(fd, inter_recv_packet_length[cmd - 0x3000])) == 0)
		return 2;

	switch(cmd) {
		case 0x3000: mapif_parse_broadcast(fd); break;
		case 0x3001: mapif_parse_WisRequest(fd); break;
		case 0x3002: mapif_parse_WisReply(fd); break;
		case 0x3003: mapif_parse_WisToGM(fd); break;
		case 0x3004: mapif_parse_Registry(fd); break;
		case 0x3005: mapif_parse_RegistryRequest(fd); break;
		case 0x3006: mapif_parse_NameChangeRequest(fd); break;
		case 0x3007: mapif_parse_accinfo(fd); break;
		// 0x3008 is unused
		case 0x3009: mapif_parse_broadcast_item(fd); break;
		default:
			if(inter_party_parse_frommap(fd) ||
				inter_guild_parse_frommap(fd) ||
				inter_storage_parse_frommap(fd) ||
				inter_pet_parse_frommap(fd) ||
				inter_homunculus_parse_frommap(fd) ||
				inter_mercenary_parse_frommap(fd) ||
				inter_elemental_parse_frommap(fd) ||
				inter_mail_parse_frommap(fd) ||
				inter_auction_parse_frommap(fd) ||
				inter_quest_parse_frommap(fd) ||
				inter_clan_parse_frommap(fd) ||
				inter_achievement_parse_frommap(fd))
				break;
			else
				return 0;
	}

	RFIFOSKIP(fd,len);
	return 1;
}
