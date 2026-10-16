/* landinglab/memory_card.c
 *
 * Holds Landing Gear's own memory card file: reading and writing the presets.
 *
 * Part of Landing Gear: src/landinglab.c includes it, in order, into one
 * translation unit, so it sees everything included before it (and mex.h,
 * events.h, landinglab_sim.h).
 *
 * Uses: menus_display, presets.
 */

///////////////////////
/// Memory card     ///
///////////////////////

// Landing Gear's own file on the memory card in slot A: one block with a
// comment for the card screen and the presets. It's read once at start (the
// event waits for it, as the lab does), written in the background one step
// a frame whenever a preset changes, and once more, waiting, on Exit.
#define CARD_SLOT 0
#define CARD_FILE "TMCE_LandingLab"
#define CARD_SIZE 8192 // one block
#define CARD_TIMEOUT_US 3000000

typedef struct CardImage
{
    char comment[CARD_COMMENT_SIZE]; // two lines of 32, at the file's start
    u32 sum;
    u32 size;
    PresetFile file;
} CardImage;
typedef char card_image_fits[sizeof(CardImage) <= 8192 ? 1 : -1]; // one block
#define CARD_READ_LEN ((sizeof(CardImage) + CARD_READ_SIZE - 1) & ~(CARD_READ_SIZE - 1))

enum card_step
{
    CARD_IDLE,
    CARD_MOUNT,
    CARD_CHECK,
    CARD_CREATE,
    CARD_STATUS,
    CARD_WRITE,
};

static u8 *card_buf; // CARD_SIZE, 32-byte aligned for the card's DMA
// frames left logging a probe line after a write, in test runs: in Dolphin,
// OSReport lines (EXI UART, which shares EXI channel 0 with slot A) have gone
// missing for a while after one
#define CARD_PROBE 180
static int card_probe;
static CARDFileInfo card_fi;
static CARDStat card_stat;
static volatile s32 card_result;
static volatile int card_done;
static int card_step;
static int card_tick;    // when the step under way started
static u8 card_mounted;
static u32 card_gen;     // goes up with every change to preset_file
static u32 card_gen_saved, card_gen_writing, card_gen_failed;

static void Card_Callback(s32 chan, s32 result)
{
    card_result = result;
    card_done = 1;
}

static void Card_Say(const char *a, const char *b, const char *c)
{
    strcpy(preset_card_desc[0], a);
    strcpy(preset_card_desc[1], b);
    strcpy(preset_card_desc[2], c);
    OSReport("LLCARD %s %s %s\n", a, b, c);
}

static u32 Card_Sum(const void *data, int n)
{
    const u8 *b = data;
    u32 h = 2166136261u;
    for (int i = 0; i < n; i++)
        h = (h ^ b[i]) * 16777619u;
    return h;
}

// Waits for the step under way: its result, or BUSY if the card never answered.
static s32 Card_Wait(void)
{
    int t0 = OSGetTick();
    while (!card_done)
        if (OSTicksToMicroseconds(OSGetTick() - t0) > CARD_TIMEOUT_US)
            return CARD_RESULT_BUSY;
    card_done = 0;
    return card_result;
}

static void Card_Error(s32 r)
{
    char line[52];
    sprintf(line, "or written (error %d), so settings last", (int)r);
    Card_Say("The memory card in slot A couldn't be read", line, "until you leave the event.");
}

// At start, waiting: the file's presets into preset_file, if it's there.
static void Card_Load(void)
{
    if (!card_buf)
    {
        void *raw = calloc(CARD_SIZE + 32);
        card_buf = (u8 *)(((u32)raw + 31) & ~31);
        Memcard_InitWorkArea();
    }
    int ok = 0;
    s32 mem, sec;
    s32 r = CARDProbeEx(CARD_SLOT, &mem, &sec);
    if (r != CARD_RESULT_READY)
    {
        Card_Say("No memory card in slot A: settings last until", "you leave the event.", "");
        Presets_Loaded(0);
        return;
    }
    card_done = 0;
    r = CARDMountAsync(CARD_SLOT, stc_memcard_work->work_area, 0, Card_Callback);
    if (r >= 0)
        r = Card_Wait();
    if (r == CARD_RESULT_READY || r == CARD_RESULT_BROKEN)
    {
        card_done = 0;
        r = CARDCheckAsync(CARD_SLOT, Card_Callback);
        if (r >= 0)
            r = Card_Wait();
        if (r == CARD_RESULT_READY)
        {
            r = CARDOpen(CARD_SLOT, CARD_FILE, &card_fi);
            if (r == CARD_RESULT_READY)
            {
                DCInvalidateRange(card_buf, CARD_READ_LEN);
                r = CARDRead(&card_fi, card_buf, CARD_READ_LEN, 0);
                CARDClose(&card_fi);
                CardImage *img = (CardImage *)card_buf;
                // older files are this one cut short: what they have
                // is kept
                u32 size = img->size;
                // up to version 4 a slot held 72 options: that layout's
                // bounds, and its slots moved into this one's
                int v4 = r == CARD_RESULT_READY && img->file.magic == PRESET_MAGIC && img->file.version < 5;
                u32 lo = v4 ? __builtin_offsetof(PresetFileV4, old_view) : __builtin_offsetof(PresetFile, old_view);
                u32 hi = v4 ? sizeof(PresetFileV4) : sizeof(PresetFile);
                if (r == CARD_RESULT_READY && size >= lo && size <= hi && img->sum == Card_Sum(&img->file, size))
                {
                    memset(preset_file, 0, sizeof(PresetFile));
                    if (v4)
                    {
                        // what a shorter, older file doesn't have reads as zero
                        memset((u8 *)&img->file + size, 0, sizeof(PresetFileV4) - size);
                        Preset_FromV4(preset_file, (PresetFileV4 *)&img->file);
                        preset_dirty = 1; // written again in the new layout
                    }
                    else
                        memcpy(preset_file, &img->file, size);
                    // Version 4 added Wavedash to the built-in presets and
                    // moved Everything to the end. A start preset saved
                    // before it keeps pointing at the same one: slot 7 was
                    // Everything, and is Wavedash now. Minimal's slot is
                    // Quiet's, and the rest stayed. Saved presets keep their
                    // settings by name, so they need nothing.
                    if (preset_file->version < 4 && preset_file->start == PS_WAVEDASH)
                    {
                        preset_file->start = PS_EVERYTHING;
                        preset_dirty = 1; // written again as version 4
                    }
                    if (preset_file->version < PRESET_VERSION)
                        preset_file->version = PRESET_VERSION;
                    ok = 1;
                    Card_Say("Kept in Landing Gear's own file on the", "memory card in slot A (1 block). Changes", "save when the menu closes.");
                }
                else
                    Card_Say("Landing Gear's file on the card in slot A", "couldn't be read, so it starts over. Changes", "save when the menu closes.");
            }
            else if (r == CARD_RESULT_NOFILE)
                Card_Say("Nothing saved on the card in slot A yet.", "Settings save to a file of 1 block there", "when the menu closes.");
            else
                Card_Error(r);
        }
        else
            Card_Error(r);
        CARDUnmount(CARD_SLOT);
    }
    else
        Card_Error(r);
    card_gen = card_gen_saved = card_gen_failed = 0;
    Presets_Loaded(ok);
}

static void Card_Stop(void)
{
    if (card_mounted)
        CARDUnmount(CARD_SLOT);
    card_mounted = 0;
    card_step = CARD_IDLE;
}

static void Card_Fail(s32 r)
{
    Card_Stop();
    card_gen_failed = card_gen_writing;
    if (r == CARD_RESULT_NOCARD || r == CARD_RESULT_WRONGDEVICE)
        Card_Say("No memory card in slot A: settings last until", "you leave the event.", "");
    else if (r == CARD_RESULT_INSSPACE || r == CARD_RESULT_NOENT)
        Card_Say("The card in slot A is full: Landing Gear", "needs 1 free block and 1 free file, so", "settings last until you leave the event.");
    else
        Card_Error(r);
}

static void Card_Write(void)
{
    card_done = 0;
    s32 r = CARDWriteAsync(&card_fi, card_buf, CARD_SIZE, 0, Card_Callback);
    if (r < 0)
    {
        CARDClose(&card_fi);
        Card_Fail(r);
        return;
    }
    card_step = CARD_WRITE;
}

// Starts writing preset_file as it is now.
static void Card_Begin(void)
{
    CardImage *img = (CardImage *)card_buf;
    memset(card_buf, 0, CARD_SIZE);
    strcpy(img->comment, "TM-CE Landing Gear");
    strcpy(img->comment + 32, "Settings presets");
    img->size = sizeof(PresetFile);
    memcpy(&img->file, preset_file, sizeof(PresetFile));
    img->sum = Card_Sum(&img->file, sizeof(PresetFile));
    DCFlushRange(card_buf, CARD_SIZE);
    card_gen_writing = card_gen;
    card_tick = OSGetTick();

    s32 mem, sec;
    s32 r = CARDProbeEx(CARD_SLOT, &mem, &sec);
    if (r == CARD_RESULT_READY)
    {
        card_done = 0;
        r = CARDMountAsync(CARD_SLOT, stc_memcard_work->work_area, 0, Card_Callback);
    }
    if (r < 0)
    {
        Card_Fail(r);
        return;
    }
    card_step = CARD_MOUNT;
}

// The next step, once the one under way is done.
static void Card_Step(void)
{
    if (card_step == CARD_IDLE)
        return;
    if (!card_done)
    {
        if (OSTicksToMicroseconds(OSGetTick() - card_tick) > CARD_TIMEOUT_US)
        {
            if (card_step == CARD_WRITE || card_step == CARD_STATUS)
                CARDClose(&card_fi);
            Card_Fail(CARD_RESULT_BUSY);
        }
        return;
    }
    s32 r = card_result;
    card_done = 0;
    card_tick = OSGetTick();
    switch (card_step)
    {
    case CARD_MOUNT:
        if (r != CARD_RESULT_READY && r != CARD_RESULT_BROKEN)
        {
            Card_Fail(r);
            return;
        }
        card_mounted = 1;
        r = CARDCheckAsync(CARD_SLOT, Card_Callback);
        if (r < 0)
            Card_Fail(r);
        else
            card_step = CARD_CHECK;
        return;
    case CARD_CHECK:
        if (r != CARD_RESULT_READY)
        {
            Card_Fail(r);
            return;
        }
        r = CARDOpen(CARD_SLOT, CARD_FILE, &card_fi);
        if (r == CARD_RESULT_READY)
            Card_Write();
        else if (r == CARD_RESULT_NOFILE)
        {
            r = CARDCreateAsync(CARD_SLOT, CARD_FILE, CARD_SIZE, &card_fi, Card_Callback);
            if (r < 0)
                Card_Fail(r);
            else
                card_step = CARD_CREATE;
        }
        else
            Card_Fail(r);
        return;
    case CARD_CREATE:
        if (r != CARD_RESULT_READY)
        {
            Card_Fail(r);
            return;
        }
        // the two comment lines at the file's start show on the card screen
        if (CARDGetStatus(CARD_SLOT, card_fi.fileNo, &card_stat) == CARD_RESULT_READY)
        {
            card_stat.commentAddr = 0;
            card_stat.iconAddr = 0xFFFFFFFF;
            card_stat.bannerFormat = 0;
            card_stat.iconFormat = 0;
            card_stat.iconSpeed = 0;
            if (CARDSetStatusAsync(CARD_SLOT, card_fi.fileNo, &card_stat, Card_Callback) >= 0)
            {
                card_step = CARD_STATUS;
                return;
            }
        }
        Card_Write();
        return;
    case CARD_STATUS:
        Card_Write(); // without the comment if it didn't take
        return;
    case CARD_WRITE:
        CARDClose(&card_fi);
        if (r != CARD_RESULT_READY)
        {
            Card_Fail(r);
            return;
        }
        Card_Stop();
        card_gen_saved = card_gen_writing;
        card_probe = CARD_PROBE;
        Card_Say("Saved in Landing Gear's own file on the", "memory card in slot A (1 block). Changes", "save when the menu closes.");
        return;
    }
}

static int Card_Wanted(void)
{
    return card_gen != card_gen_saved && card_gen != card_gen_failed;
}

// Each frame: start a write when the presets changed, and move the one
// under way along.
static void Card_Update(void)
{
    if (preset_dirty)
    {
        preset_dirty = 0;
        card_gen++;
    }
    if (card_step == CARD_IDLE && Card_Wanted())
        Card_Begin();
    Card_Step();
}

// Leaving the event: finish what's under way and write what's left, waiting.
static void Card_Flush(void)
{
    for (int pass = 0; pass < 2; pass++)
    {
        if (card_step == CARD_IDLE)
        {
            Card_Update();
            if (card_step == CARD_IDLE)
                break;
        }
        while (card_step != CARD_IDLE)
            Card_Step();
    }
}

// The settings as they are now into User Custom, if they changed. A preset
// loaded and left as it is doesn't count: User Custom keeps what was made by
// hand, so going back to it gets that back.
static void Presets_KeepUser(void)
{
    PresetSlot now;
    Preset_Capture(&now);
    if (preset_active != PS_USER && Preset_Same(&now, preset_as_set))
        return;
    if (preset_active != PS_USER)
    {
        OSReport("LLPRESET %s changed by hand: kept as User Custom\n", preset_names[preset_active]);
        preset_active = PS_USER;
        Options_Presets[PROPT_PICK].val = PS_USER;
        Preset_Describe(PS_USER);
    }
    if (!Preset_Same(&now, &preset_file->slot[PS_USER]))
    {
        memcpy(&preset_file->slot[PS_USER], &now, sizeof(now));
        preset_dirty = 1;
    }
}
