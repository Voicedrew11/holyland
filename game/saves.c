/* saves.c: PC-only. The game's saves without a memory card: stands in for save/save_card.c (which the PC
   build leaves out, with save/mc.c under it) and keeps the three saves as files in the save folder
   (port/include/port/save_files.h).

   The save and load screens (ui/trade.c, ui/menu_screens.c) call the same SaveCard_ functions as on the
   PS2 and get the same kind of answers, so their own logic and messages run as they are: the save folder
   answers as a formatted card in the first port with room to spare, the second port is always empty
   (the screens' patches skip choosing between the two). A task the screens start is done in one go at
   the next SaveCard_Update, which reports it finished or failed as the card's tasks did.

   - The save list is read from the save files' records. A damaged file is read from its backup; with no
     whole backup either it is listed as empty, so that the other saves can still be loaded.
   - A save writes its file whole (the record the list shows, then the data Game_WriteSaveImage made) or
     not at all: a failed save leaves the save it was to replace as it was.
   - A load puts the data in save_image and leaves the main loop as a card load did.
   The automap and the hands keep their state per save beside it (port_automap_save_file,
   port_hands_save_file), and the mods' plugins are told (port_mod_save_file).

   Runs on the game's thread. Its state is the program's .bss, so a save state holds it. */
#include <string.h>

#include "common.h"
#include "core/game.h"
#include "player/player.h"
#include "save/save_card.h"
#include "port/automap.h"
#include "port/hands.h"
#include "port/mod_api.h"
#include "port/platform.h"
#include "port/save_files.h"

#define FOLDER_CARD 0         /* the save folder answers as the card in the first port */
#define CARD_TYPE_PS2 2       /* CardStatus.card_type of a usable card */
#define CARD_CLUSTERS 8000    /* a card's room, in the 1 KB clusters the screens count */
#define FOLDER_CLUSTERS 0x96  /* what a card's first save took for the game's folder */
#define SAVE_CLUSTERS 0x5A    /* and each save */
#define CHECK_NOT_CHECKED 1   /* CardStatus.check_state */
#define CHECK_FAILED 2

/* the messages SaveCard_SetFailureMessage picks for a usable card whose task failed (CardRecord.message) */
#define MESSAGE_NO_CARD 2
#define MESSAGE_FORMAT_FAILED 6
#define MESSAGE_SAVE_FAILED 9
#define MESSAGE_LOAD_FAILED 7
#define MESSAGE_LIST_FAILED 10

extern int save_loaded;
extern GameStatus game_status;
extern PlayerState player_state;
extern PlayTime current_play_time;

typedef struct {
    s16 kind;         /* CARD_TASK_ */
    s16 last_kind;    /* the kind that was started: kind becomes CARD_TASK_FAILED on a failure */
    s32 card;
    s32 save;         /* which of the three saves */
    s32 *data;        /* the save image to write or read into; its first word is its size */
    s32 size;         /* a load's room in data */
    s8 check_running; /* SaveCard_IsCheckRunning: set while the check of the cards runs */
} Task;

typedef struct {
    s8 wanted;    /* SaveCard_Open asked for the cards to be checked, again and again while open */
    s8 keep_open; /* cleared by SaveCard_Close: the next check finished ends the card work */
    s8 done;      /* a check has finished since SaveCard_Open */
} Check;

static CardRecord cards[2];
static SaveInfo infos[PORT_SAVE_COUNT];
static Task task;
static Check check;

/* ---- The cards ------------------------------------------------------------------------------- */
static void forget_card(CardRecord *card) {
    card->status.bytes.check_state = CHECK_NOT_CHECKED;
    card->status.bytes.card_type = 0;
    card->free_clusters = 0;
    card->present = 0;
    card->formatted = 0;
    card->has_save_list = 0;
    card->has_save_folder = 0;
    card->no_card = 0;
    card->icon_missing = 0;
    card->load_failed = 0;
    card->damaged = 0;
    card->message = 0;
}

static int card_usable(s32 index) {
    return index == FOLDER_CARD && port_saves_available();
}

/* The room the screens are told is free: a card's, less what the saves listed would take on one. Always
   room for all three; it goes down with each new save, as on a card, because the save screen redraws its
   list when it changes (ui/trade.c, after a save) */
static s32 free_clusters(void) {
    s32 saves = 0;
    s32 i;

    for (i = 0; i < PORT_SAVE_COUNT; i++) {
        saves += infos[i].exists != 0;
    }
    return CARD_CLUSTERS - (saves > 0 ? FOLDER_CLUSTERS : 0) - saves * SAVE_CLUSTERS;
}

/* What the check of both cards finds: the save folder in the first port, ready, and nothing in the
   second (nor in the first with --saves none) */
static void check_cards(void) {
    s32 i;

    for (i = 0; i < 2; i++) {
        CardRecord *card = &cards[i];

        card->status.bytes.check_state = 0;
        if (card_usable(i)) {
            card->no_card = 0;
            card->present = 1;
            card->formatted = 1;
            card->status.bytes.card_type = CARD_TYPE_PS2;
            card->free_clusters = free_clusters();
            card->has_save_folder = 1;
        } else {
            card->status.bytes.card_type = 0;
            card->present = 0;
            card->formatted = 0;
            card->has_save_folder = 0;
            card->has_save_list = 0;
            card->message = MESSAGE_NO_CARD;
        }
    }
}

/* ---- The tasks ------------------------------------------------------------------------------- */
static s32 start_task(s16 kind, s32 card, s32 save) {
    task.kind = task.last_kind = kind;
    task.card = card;
    task.save = save;
    return 0;
}

/* A task failed: the card is marked failed (the check waits until a SaveCard_StartCheck), the task
   stays failed (SaveCard_CheckBusy says busy, SaveCard_Update -1) until the screens move on */
static s32 fail(void) {
    CardRecord *card = &cards[task.card & 1];

    task.kind = CARD_TASK_FAILED;
    card->status.bytes.check_state = CHECK_FAILED;
    switch (task.last_kind) {
    case CARD_TASK_FORMAT:
        card->message = MESSAGE_FORMAT_FAILED;
        break;
    case CARD_TASK_READ_LIST:
        card->message = MESSAGE_LIST_FAILED;
        break;
    case CARD_TASK_SAVE:
        card->message = MESSAGE_SAVE_FAILED;
        break;
    case CARD_TASK_LOAD:
        card->message = MESSAGE_LOAD_FAILED;
        break;
    default:
        card->message = 1;
        break;
    }
    return -1;
}

/* A save that cannot be read (neither its file nor the file's backup is whole) is listed as empty: on a card
   it was "damaged", and the load screen then refused every save on the card (SaveCard_CheckSavesIntact) */
static void read_list(void) {
    s32 i;

    for (i = 0; i < PORT_SAVE_COUNT; i++) {
        SaveInfo *info = &infos[i];
        PortSaveState state = port_save_record(i, (uint8_t *)info);

        if (state != PORT_SAVE_OK) {
            memset(info, 0, sizeof(*info));
        }
        if (state == PORT_SAVE_DAMAGED) {
            port_event("saves: save %d is damaged and has no whole backup; listed as empty", i + 1);
        }
        info->exists = state == PORT_SAVE_OK;
        info->damaged = 0;
    }
}

static s32 do_read_list(void) {
    CardRecord *card = &cards[task.card & 1];

    if (!card->present || card->status.bytes.card_type != CARD_TYPE_PS2) {
        return fail();
    }
    read_list();
    card->has_save_list = 1;
    card->free_clusters = free_clusters();
    return 1;
}

/* What the save list shows of the game being saved (save_card.c's SaveCard_FillSaveInfo): the area's
   name, the player's experience, level, hit and magic points with their maximums, two more words and
   gold, and the play time */
static void fill_record(SaveInfo *info) {
    const s32 *words = (const s32 *)&player_state;

    memset(info, 0, sizeof(*info));
    info->exists = 1;
    info->numbers[0] = game_status.save_area;
    info->numbers[1] = words[20];
    info->numbers[2] = words[21];
    info->numbers[3] = words[45];
    info->numbers[4] = words[22];
    info->numbers[5] = words[46];
    info->numbers[6] = words[23];
    info->numbers[7] = words[10];
    info->numbers[8] = words[11];
    info->numbers[9] = words[19];
    info->play_time = current_play_time;
}

static s32 do_save(void) {
    CardRecord *card = &cards[task.card & 1];
    SaveInfo record;

    if (!card_usable(task.card) || task.data == NULL) {
        return fail();
    }
    fill_record(&record);
    if (!port_save_write(task.save, (const uint8_t *)&record, (const uint8_t *)task.data, (size_t)(u32)task.data[0])) {
        return fail();
    }
    infos[task.save] = record;
    card->has_save_list = 1;
    card->free_clusters = free_clusters();
    port_automap_save_file(task.save + 1, 1);
    port_hands_save_file(task.save + 1, 1);
    port_mod_save_file(task.save + 1, 1); /* the mods' plugins: HL_EVENT_SAVE_WRITTEN (port/include/port/mod_api.h) */
    return 1;
}

static s32 do_load(void) {
    CardRecord *card = &cards[task.card & 1];
    SaveInfo record;
    size_t size;

    if (!card_usable(task.card) ||
        port_save_read(task.save, (uint8_t *)&record, (uint8_t *)task.data, (size_t)task.size, &size) !=
            PORT_SAVE_OK) {
        card->load_failed = 1;
        return fail();
    }
    port_automap_save_file(task.save + 1, 0);
    port_hands_save_file(task.save + 1, 0);
    port_mod_save_file(task.save + 1, 0); /* the mods' plugins: HL_EVENT_SAVE_LOADED */
    save_loaded = 1;
    game_status.bit9 = 1;
    return 1;
}

/* The card's second look through both ports: the same answers as the check, the list there to read */
static s32 do_rescan(void) {
    check_cards();
    if (cards[FOLDER_CARD].present) {
        cards[FOLDER_CARD].has_save_list = 1;
    }
    return 1;
}

static int step_task(void) {
    int result;

    switch (task.kind) {
    case CARD_TASK_SAVE:
        result = do_save();
        break;
    case CARD_TASK_LOAD:
        result = do_load();
        break;
    case CARD_TASK_READ_LIST:
        result = do_read_list();
        break;
    case CARD_TASK_RESCAN:
        result = do_rescan();
        break;
    case CARD_TASK_FAILED:
        result = -1;
        break;
    case CARD_TASK_FORMAT: /* the folder needs none */
    case CARD_TASK_NONE:
    default:
        result = 1;
        break;
    }
    if (result == 1) {
        task.kind = CARD_TASK_NONE;
    }
    return result;
}

/* ---- What the screens call (save/save_card.h) ------------------------------------------------ */
s32 SaveCard_Init(void) {
    memset(&task, 0, sizeof(task));
    return 0;
}

s32 SaveCard_Open(void) {
    check.done = 0;
    check.wanted = 1;
    check.keep_open = 1;
    forget_card(&cards[0]);
    forget_card(&cards[1]);
    SaveCard_ClearSaveInfos();
    return 0;
}

int SaveCard_Update(void) {
    if (check.wanted && cards[0].status.bytes.check_state != CHECK_FAILED &&
        cards[1].status.bytes.check_state != CHECK_FAILED) {
        check_cards();
        task.check_running = 1;
        if (task.kind == CARD_TASK_CHECK) {
            task.kind = CARD_TASK_NONE;
            task.check_running = 0;
            return 1;
        }
        if (task.kind == CARD_TASK_NONE) {
            if (!check.keep_open) {
                check.done = 0;
                check.wanted = 0;
                return -1;
            }
            check.done = 1;
            task.check_running = 0;
            return 1;
        }
    }
    return step_task();
}

s32 SaveCard_StartCheck(s8 reset) {
    s32 i;

    if (task.kind != CARD_TASK_NONE && task.kind != CARD_TASK_FAILED) {
        return -1;
    }
    task.kind = task.last_kind = CARD_TASK_CHECK;
    for (i = 0; i < 2; i++) {
        if (cards[i].status.bytes.check_state != 0) {
            cards[i].status.bytes.check_state = CHECK_NOT_CHECKED;
        }
    }
    if (reset) {
        forget_card(&cards[0]);
        forget_card(&cards[1]);
    }
    return 0;
}

s32 SaveCard_StartReadList(s32 card, char must_exist) {
    (void)must_exist; /* the folder's list is always there to read */
    if (task.kind != CARD_TASK_NONE) {
        return -1;
    }
    return start_task(CARD_TASK_READ_LIST, card, 0);
}

s32 SaveCard_StartSave(s32 card, s32 file, s32 *data) {
    if (task.kind != CARD_TASK_NONE || file < 0 || file >= PORT_SAVE_COUNT) {
        return -1;
    }
    task.data = data;
    return start_task(CARD_TASK_SAVE, card, file);
}

s32 SaveCard_StartLoad(s32 card, s32 file, s32 *data, s32 size) {
    if (task.kind != CARD_TASK_NONE || data == NULL || file < 0 || file >= PORT_SAVE_COUNT) {
        return -1;
    }
    task.data = data;
    task.size = size;
    return start_task(CARD_TASK_LOAD, card, file);
}

s32 SaveCard_StartFormat(s32 card) {
    if (task.kind != CARD_TASK_NONE) {
        return -1;
    }
    return start_task(CARD_TASK_FORMAT, card, 0);
}

s32 SaveCard_StartRescan(void) {
    return start_task(CARD_TASK_RESCAN, task.card, 0);
}

void SaveCard_Close(void) {
    check.keep_open = 0;
}

s32 SaveCard_CheckBusy(void) {
    return task.kind == CARD_TASK_NONE ? 0 : -1;
}

SaveInfo *SaveCard_GetSaveInfo(s32 index) {
    return &infos[index];
}

s32 SaveCard_HasCard(s32 index) {
    return cards[index].status.bytes.card_type != 0;
}

s8 SaveCard_GetSaveState(s32 index) {
    return infos[index].damaged ? -1 : infos[index].exists;
}

s32 SaveCard_CheckSavesIntact(void) {
    s32 i;

    for (i = 0; i < PORT_SAVE_COUNT; i++) {
        if (infos[i].damaged) {
            return -1;
        }
    }
    return 1;
}

s32 SaveCard_HasRoomForFolder(s32 index) {
    return cards[index].free_clusters >= 0x96 ? 1 : -1;
}

s32 SaveCard_HasRoomFor(s32 index, s32 count, s32 extra) {
    s32 needed = count * 0x5A + (extra ? 0x96 : 0);

    return cards[index].free_clusters >= needed ? 1 : -1;
}

s32 SaveCard_GetFreeClusters(s32 index) {
    return cards[index].free_clusters;
}

s32 SaveCard_GetSaveMask(void) {
    s32 mask = 0;
    s32 i;

    for (i = 0; i < PORT_SAVE_COUNT; i++) {
        if (infos[i].damaged) {
            return 0xFF;
        }
        if (infos[i].exists) {
            mask |= 1 << i;
        }
    }
    return mask;
}

void SaveCard_ClearSaveInfos(void) {
    memset(infos, 0, sizeof(infos));
}

void SaveCard_ResetCard(s32 index) {
    forget_card(&cards[index]);
}

void SaveCard_ClearLoadFailed(s32 index) {
    cards[index].load_failed = 0;
}

s8 SaveCard_GetLoadFailed(s32 index) {
    return cards[index].load_failed;
}

s32 SaveCard_IsCheckDone(void) {
    return check.done != 0;
}

s32 SaveCard_IsChecking(void) {
    return task.kind == CARD_TASK_CHECK;
}

s8 SaveCard_IsCheckRunning(void) {
    return task.check_running;
}

s8 SaveCard_IsPresent(s32 index) {
    return cards[index].present;
}

s8 SaveCard_IsFormatted(s32 index) {
    return cards[index].formatted;
}

s8 SaveCard_HasSaveFolder(s32 index) {
    return cards[index].has_save_folder;
}

s8 SaveCard_IsDamaged(s32 index) {
    return cards[index].damaged;
}

void SaveCard_Cancel(s32 index) {
    (void)index;
    cards[0].status.bytes.check_state = CHECK_NOT_CHECKED;
    cards[1].status.bytes.check_state = CHECK_NOT_CHECKED;
    task.kind = CARD_TASK_NONE;
}

s32 SaveCard_GetCardProblem(s32 index) {
    const CardRecord *card = &cards[index];

    if (card->status.bytes.check_state != 0) {
        return 1;
    }
    return card->status.bytes.card_type != CARD_TYPE_PS2 ? 2 : 0;
}

s32 SaveCard_GetSavesState(void) {
    s32 i;

    for (i = 0; i < PORT_SAVE_COUNT; i++) {
        if (infos[i].damaged) {
            return -1;
        }
        if (infos[i].exists) {
            return 1;
        }
    }
    return 0;
}

s32 SaveCard_HasAnySave(void) {
    s32 i;

    for (i = 0; i < PORT_SAVE_COUNT; i++) {
        if (infos[i].exists) {
            return 1;
        }
    }
    return 0;
}
