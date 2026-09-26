#ifndef _ITEMS_H_
#define _ITEMS_H_

#define RANDOM_RELOCATOR 1003
#define LORD_REVIVE_FRUIT_ITEM 1117   /* "Revives a Leader" in the game's Item table: the resurrection fruit */
#define ADVANCE_RELOCATOR 1004

#define MIGRATION_SCROLL 1275


#define SHIELD_4H  1146
#define SHIELD_8H  1051
#define SHIELD_12H 1462
#define SHIELD_1D  1052
#define SHIELD_3D  1053
#define SHIELD_7D  1287
#define SHIELD_14D 1288

/* "Anti-espionnage" in the French client: hides the account from scout reports
 * without blocking attacks the way a shield does. Same _MSG_REQUEST_USEITEM /
 * _MSG_RESP_USEITEM mechanism as shields, response has the same extra fields
 * (quantity, item_id, begin_time, duration) - captured using all 5 at once. */
#define ANTISCOUT_4H  1147
#define ANTISCOUT_8H  1156
#define ANTISCOUT_1D  1054
#define ANTISCOUT_3D  1055
#define ANTISCOUT_7D  1056

#define FAMILIAR_MAGMA_LORD  0x0012

#define FOOD_5K 0x0492 
#define FOOD_30K 0x03F1 
#define FOOD_150K 0x03F6 
#define FOOD_500K 0x03FB 
#define FOOD_2M 0x0400 
#define FOOD_6M 0x0445 
#define FOOD_20M 0x044A 
#define FOOD_60M 0x044F 
/* Resource items the game has besides the plain ones (same record in its Item table; the game's own "use the bag" took 0x0587) */
#define FOOD_20K  0x0490
#define FOOD_10K  0x04A0
#define FOOD_50K  0x04DD
#define FOOD_100K 0x04DE
#define FOOD_250K 0x0587

#define STONE_3K 0x0493 
#define STONE_10K 0x03F2 
#define STONE_50K 0x03F7 
#define STONE_150K 0x03FC 
#define STONE_500K 0x0401 
#define STONE_1_5M 0x0446 
#define STONE_5M 0x044B 
#define STONE_15M 0x0450 
#define STONE_1K   0x047F
#define STONE_5K   0x04A9
#define STONE_25K  0x04DF
#define STONE_250K 0x0588

#define TIMBER_3K 0x0494 
#define TIMBER_10K 0x03F3 
#define TIMBER_50K 0x03F8 
#define TIMBER_150K 0x03FD 
#define TIMBER_500K 0x0402 
#define TIMBER_1_5M 0x0447 
#define TIMBER_5M 0x044C 
#define TIMBER_15M 0x0451 
#define TIMBER_1K   0x0480
#define TIMBER_5K   0x04B2
#define TIMBER_25K  0x04E0
#define TIMBER_250K 0x0589

#define ORE_3K 0x0495 
#define ORE_10K 0x03F4 
#define ORE_50K 0x03F9 
#define ORE_150K 0x03FE 
#define ORE_500K 0x0403 
#define ORE_1_5M 0x0448 
#define ORE_5M 0x044D 
#define ORE_15M 0x0452 
#define ORE_1K   0x0481
#define ORE_5K   0x04BB
#define ORE_25K  0x04E1
#define ORE_250K 0x058A


#define GOLD_3K 0x03F5 
#define GOLD_15K 0x03FA 
#define GOLD_50K 0x03FF 
#define GOLD_200K 0x0404 
#define GOLD_600K 0x0449 
#define GOLD_2M 0x044E 
#define GOLD_6M 0x0453 
#define GOLD_6K   0x04C0
#define GOLD_9K   0x04E2
#define GOLD_30K  0x04E3
#define GOLD_100K 0x04E4


#define BRIGHT_TALENT_ORB 3603
#define SPEED_UP_30_MINUTE 1041
#define SPEED_UP_60_MINUTE 1042
#define SPEED_UP_3_HOUR 1081

#define SPEED_UP_MERGING_3_HOUR 1334
#define SPEED_UP_RESEARCH_3_HOUR 1262

#define ANIMA 3709

#define WITHDRAW_SQUAD 1001

#endif