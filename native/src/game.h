#pragma once
/* Addresses in MEGAMAN.EXE (2001 PC CD build, no ASLR, image base 0x400000).
 * Names match RE-notes/ghidra/symbols.csv. */

#define G_IAT_GETTICKCOUNT      0x508328u   /* import slot for kernel32!GetTickCount */
#define G_GTE_H_FLOAT           0x77EBDCu   /* float: projection plane distance */
#define G_FRAME_LIMITER_TH      0x55BDB4u   /* float: next-frame threshold in ms */
#define G_FRAME_STEP            0x77EAC1u   /* byte: 2 = 30 fps */
#define G_FRAME_MS              0x50A398u   /* double: 16.667 */
#define G_GFX_CUR_CTX           0x77EA74u   /* gfx_ctx*: +0x78 = HUD bucket */
#define G_FONT_WIDTHS           0x52DFA0u   /* u8[]: glyph advance by font tile code */
#define G_INTRO_PLAYING         0x551FA8u   /* byte: 1 while CAPCOM.AVI plays */

/* camera */
#define G_CAM_PITCH             0x77EA08u   /* u16, 12-bit angle (4096 = 360 deg) */
#define G_CAM_YAW               0x77EA0Au
#define G_CAM_RIG_YAW           0x77E960u   /* follow-cam azimuth = eased(facing + 0x800) */
#define G_CAM_TARGET_X          0x77E998u   /* s32 16.16 */
#define G_CAM_TARGET_Z          0x77E9A0u
#define G_CAM_POS_X             0x56F182u   /* s32 16.16 */
#define G_CAM_POS_Z             0x56F186u

/* player (struct at 0x5F8364) */
#define G_PLAYER_STATE          0x5F836Cu   /* +0x08: bit 0x800 busy/roll, bit 0x01000000 roll side (1 = left) */
#define G_PLAYER_X              0x5F8380u   /* s32 16.16 */
#define G_PLAYER_Z              0x5F8388u
#define G_PLAYER_FACING         0x5F83B8u   /* 16.16 angle; 12-bit integer part in the high word */

/* hook sites */
#define G_SITE_H_DEFAULT        0x4EF097u   /* in gte_set_h_default, right after H = 1000.0 */
#define G_SITE_H_SET            0x4EF0E1u   /* in gte_set_h, right after fst H */
#define G_SITE_WALKER_CALL      0x4A70E5u   /* game_frame: call gfx_walk_display_list (once per frame) */
#define G_SITE_INTRO_CALL       0x4A6F7Eu   /* call intro_play_avi */
#define G_FN_CAM_APPLY          0x42961Bu   /* cam_apply_angles: EAX pos*, EDX dist, EBX yaw, ECX -pitch */
#define G_FN_CAM_SET_B          0x4298F6u   /* cam_set_angles_b: yaw in EDX */
#define G_FN_PLAYER_MOVE        0x4B53D3u   /* runs each frame while the player has control */
#define G_SITE_PLAYER_TURN_RET  0x4B55B6u   /* player_turn epilogue: mov eax,ecx; pop edi; pop esi; ret */
