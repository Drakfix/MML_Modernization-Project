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
#define G_CAM_RIG               0x77E950u   /* camera rig: +4 active (1), +5 mode (0 follow), +6 scripted */
#define G_FN_WORLD_COLLISION    0x42CE61u   /* world_line_collision: point vs level cells, 0 = free */
#define G_CAM_COLLISION_PARAMS  0x5110B0u   /* p3 the follow camera passes to it */
#define G_CAM_RIG_YAW           0x77E960u   /* follow-cam azimuth = eased(facing + 0x800) */
#define G_CAM_TARGET_X          0x77E998u   /* s32 16.16 */
#define G_CAM_TARGET_Z          0x77E9A0u
#define G_CAM_POS_X             0x56F182u   /* s32 16.16 */
#define G_CAM_POS_Z             0x56F186u

/* player (struct at 0x5F8364) */
#define G_PLAYER                0x5F8364u   /* player object; offsets below (RE-notes 7.11.1) */
#define PL_STATE                0x09        /* u8: 0 idle, 1 walk, 2 run, ..., 9 cartwheel */
#define PL_SUBSTATE             0x0A
#define PL_VARIANT              0x0B
#define PL_VEL_FWD              0x44        /* s16 */
#define PL_VEL_SIDE             0x48        /* s16 */
#define PL_AIRBORNE             0x74
#define PL_ANIM_ENTRY           0xA4        /* u8 entry index; +0xA5 hold ticks, +0xA6 event, +0xA7 flags (0xFF = ended) */
#define PL_ANIM_HOLD            0xA5
#define PL_ANIM_FLAGS           0xA7
#define PL_ANIM_ID              0xAC
#define PL_AIM_PITCH            0x108       /* s16 aim up/down in the aim state (clamped +-0x1C0) */
#define PL_INPUT_FLAGS          0xBD        /* 0x40 input locked, 0x20 scripted input */
#define PL_RUN_DIR              0x112
#define PL_MOVE_ANGLE           0x116       /* s16 travel offset from facing */
#define PL_HELD                 0x11C       /* u16 held buttons (PS1 bits) */
#define PL_MASK_WALK            0x134       /* u16 walk-modifier button mask (0x2000) */
#define PL_MASK_TALK            0x136       /* u16 talk/check mask: the same button as walk */
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
#define G_RET_CAM_COMMON_BLEND  0x42A2BBu   /* return addresses of camera_apply_common's two cam_apply_angles */
#define G_RET_CAM_COMMON        0x42A2E0u   /* calls (the gameplay camera; other callers: cam_follow_* etc.) */
#define G_FN_CAM_SET_B          0x4298F6u   /* cam_set_angles_b: yaw in EDX */
#define G_FN_PLAYER_MOVE        0x4B53D3u   /* runs each frame while the player has control */
#define G_FN_PLAYER_UPDATE      0x4A9A31u   /* player_update(EAX = object): pre + state handlers */
#define G_FN_PLAYER_APPLY_VEL   0x4B527Bu   /* player_apply_velocity(EAX p, DX fwd, BX vert, CX side) along facing - +0x116 */
#define G_SUPPORT_OBJ           0x5F8A58u   /* second player-like object (input from 0x572D32) */
#define G_TARGET                0x5505E8u   /* object*: nearest enemy, picked every frame (target_and_hits_update 0x4E0405) */
#define G_TARGET_FLAGS          0x559A40u   /* bit 1: targeting allowed (state 14 checks it) */
#define G_FN_PAD_POLL           0x4F8E15u   /* pad_poll: joystick half, then keyboard half */
#define G_SITE_JOY_GATE         0x4F8E26u   /* pad_poll: cmp dword [G_JOY_DEVICE],0 */
#define G_SITE_PAD_POLL_KB      0x4F8F22u   /* pad_poll: start of the keyboard half */
#define G_JOY_DEVICE            0x551F40u   /* IDirectInputDevice* joystick (Xidi) or NULL */
#define G_PAD_HELD              0x5708DAu   /* u16 pad held buttons (active high), copied to player +0x11C */
#define G_PAD_BUF_PTR           0x887A48u   /* -> pad state 0x5708B4: status, type, u16 buttons (active low) */
#define G_PAD_MASKS             0x551F84u   /* u16[14]: game button index -> PS1 bit (Up Down Left Right Tri X Sq O L1 L2 R1 R2 Start Select) */
#define G_JOY_BINDINGS          0x887A34u   /* {dinput button, game button}[9] */
#define G_JOY_BINDING_COUNT     9
#define G_SITE_PLAYER_TURN_RET  0x4B55B6u   /* player_turn epilogue: mov eax,ecx; pop edi; pop esi; ret */
