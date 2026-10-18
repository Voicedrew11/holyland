/* rodata_0043EB64.c: the game's data from the decomp's rodata_0043EB64.rodata.s, defined in C for the PC (splat had it in asm) */
#include "common.h"
#include "core/load.h"
#include "entity/entity.h"
#include "gfx/packet.h"
#include "sound/sound_game.h"

/* 0x0043EB68 */
EntityRangeWeights near_point_range_weights = { .weights = {
    10, 20, 30, 40, 50, 60, 70, 80,
    90, 100,
} };

/* 0x0043EB90 */
GsDataBlock offscreen_square_corners __attribute__((aligned(16))) = { .corner0_xy = 0x780000007800LL, .corner0_z = 0xFFFFFFLL, .corner1_xy = 0x880000008800LL, .corner1_z = 0xFFFFFFLL };

/* 0x0043EBB0 */
char clut_dump_path[32] = "host0:/cs0-2/take/temp/clut.raw";

/* 0x0043EBD0 */
char vram_dump_path[32] = "host0:/cs0-2/take/temp/vram.raw";

/* 0x0043EBF0 */
char screenshot_bmp_path_format[48] = "host0:/cs0-2/KF4/output/screenshot/%4.4d.bmp";

/* 0x0043EC20 */
char screenshot_raw_path_format[48] = "host0:/cs0-2/KF4/output/screenshot/%4.4d.raw";

/* 0x0043EC50 */
char data_dump_path_format[48] = "host0:/cs0-2/KF4/output/dump/%4.4d.dat";

/* 0x0043EC80 */
char bgm_sq_format[16] = "snd/bgm%03d.sq";

/* 0x0043EC90 */
char bgm_hd_format[16] = "snd/bgm%03d.hd";

/* 0x0043ECA0 */
char bgm_bd_format[16] = "snd/bgm%03d.bd";

/* 0x0043ECB0 */
char se_com_hd_path[16] = "snd/se_com.hd";

/* 0x0043ECC0 */
char se_com_bd_path[16] = "snd/se_com.bd";

/* 0x0043ECD0 */
char sfx_com_hd_path[16] = "snd/sfx_com.hd";

/* 0x0043ECE0 */
char sfx_com_bd_path[16] = "snd/sfx_com.bd";

/* 0x0043ECF0 */
char map_bank_hd_path[16] = "snd/map000.hd";

/* 0x0043ED00 */
char map_bank_bd_path[16] = "snd/map000.bd";

/* 0x0043ED10 */
char sw_bank_hd_format[16] = "snd/sw%03d.hd";

/* 0x0043ED20 */
char sw_bank_bd_format[16] = "snd/sw%03d.bd";

/* 0x0043ED30 */
char mg_bank_hd_format[16] = "snd/mg%03d.hd";

/* 0x0043ED40 */
char mg_bank_bd_format[16] = "snd/mg%03d.bd";

/* 0x0043ED50 */
int (*const bgm_load_steps[14])(LoadJob *, s32) = {
    (int (*)(LoadJob *, s32))Sound_BgmFadeOut,
    Sound_BgmReadBody,
    (int (*)(LoadJob *, s32))Sound_BgmCloseBody,
    Sound_BgmReadHeader,
    (int (*)(LoadJob *, s32))Sound_BgmCloseHeader,
    (int (*)(LoadJob *, s32))Sound_BgmLoadVoice,
    (int (*)(LoadJob *, s32))Sound_BgmFreeSampleBuffer,
    Sound_BgmReadSequence,
    (int (*)(LoadJob *, s32))Sound_BgmCloseSequence,
    (int (*)(LoadJob *, s32))Sound_BgmStartSequence,
    (int (*)(LoadJob *, s32))Sound_BgmStartCue,
    (int (*)(LoadJob *, s32))Sound_BgmApplyReverb,
    (int (*)(LoadJob *, s32))Sound_BgmFadeIn,
};

/* 0x0043ED88 */
int (*entity_bank_load_steps[2])(SoundJob *job) = { Sound_EntityBankLoadVoice, Sound_EntityBankFreeSampleBuffer };

/* 0x0043ED90 */
int (*map_bank_load_steps[6])(SoundJob *job) = {
    Sound_MapBankNoOp,
    Sound_MapBankLoadVoice,
    Sound_MapBankFreeSampleBuffer,
    Sound_MapBankFinishUnlessFirst,
    Sound_MapBankAddReverb,
    Sound_MapBankApplyReverb,
};

/* 0x0043EDA8 */
s32 (*const sound_file_load_steps[4])(void) = {
    (s32 (*)(void))Sound_ReadJobFile,
    (s32 (*)(void))Sound_ReadRequestFile,
    (s32 (*)(void))Sound_FileLoadVoice,
    (s32 (*)(void))Sound_FileLoadFreeSampleBuffer,
};
