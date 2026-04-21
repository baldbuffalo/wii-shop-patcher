#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include <malloc.h>
#include <ogcsys.h>
#include <gccore.h>
#include <wiiuse/wpad.h>
#include <ogc/es.h>
#include <fat.h>

// -----------------------------------------------------------------------
// Config
// -----------------------------------------------------------------------
#define REPLACE_DOL     "sd:/replace.dol"
#define BACKUP_DIR      "sd:/shop_backup"

#define SHOP_TITLE_USA      0x0001000248414241ULL
#define SHOP_TITLE_EUR      0x0001000248414245ULL
#define SHOP_TITLE_JPN      0x000100024841424aULL
#define SHOP_TITLE_VWII_USA 0x000100024841424bULL
#define SHOP_TITLE_VWII_EUR 0x000100024841424cULL
#define SHOP_TITLE_VWII_JPN 0x000100024841424dULL

static const int PREFERRED_CIOS[] = { 249, 250, 248, 247, 246, 58, 236, -1 };

// -----------------------------------------------------------------------
// Video
// -----------------------------------------------------------------------
static GXRModeObj *rmode = NULL;
static void       *xfb   = NULL;

static void init_video(void) {
    VIDEO_Init();
    rmode = VIDEO_GetPreferredMode(NULL);
    xfb   = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
    console_init(xfb, 20, 20, rmode->fbWidth, rmode->xfbHeight,
                 rmode->fbWidth * VI_DISPLAY_PIX_SZ);
    VIDEO_Configure(rmode);
    VIDEO_SetNextFramebuffer(xfb);
    VIDEO_SetBlack(FALSE);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    if (rmode->viTVMode & VI_NON_INTERLACE) VIDEO_WaitVSync();
}

// -----------------------------------------------------------------------
// cIOS autodetect
// -----------------------------------------------------------------------
static int autodetect_and_load_cios(void) {
    u32 num_titles = 0;
    if (ES_GetNumTitles(&num_titles) < 0 || num_titles == 0) {
        printf("  [!] ES_GetNumTitles failed\n");
        return -1;
    }

    uint64_t *title_list = (uint64_t *)memalign(32, sizeof(uint64_t) * num_titles);
    if (!title_list) { printf("  [!] Out of memory\n"); return -1; }
    if (ES_GetTitles(title_list, num_titles) < 0) {
        free(title_list); return -1;
    }

    int ios_slots[256], ios_count = 0;
    for (u32 i = 0; i < num_titles; i++) {
        uint32_t hi = (uint32_t)(title_list[i] >> 32);
        uint32_t lo = (uint32_t)(title_list[i] & 0xFFFFFFFF);
        if (hi == 0x00000001 && lo >= 3 && lo <= 255)
            ios_slots[ios_count++] = (int)lo;
    }
    free(title_list);

    int ordered[256], ordered_count = 0;
    uint8_t added[256] = {0};
    for (int p = 0; PREFERRED_CIOS[p] != -1; p++) {
        int slot = PREFERRED_CIOS[p];
        for (int j = 0; j < ios_count; j++) {
            if (ios_slots[j] == slot && !added[slot]) {
                ordered[ordered_count++] = slot;
                added[slot] = 1;
                break;
            }
        }
    }
    for (int s = 255; s >= 3; s--) {
        if (added[s]) continue;
        for (int j = 0; j < ios_count; j++) {
            if (ios_slots[j] == s) {
                ordered[ordered_count++] = s;
                added[s] = 1;
                break;
            }
        }
    }

    WPAD_Shutdown();
    int loaded = -1;
    for (int i = 0; i < ordered_count; i++) {
        printf("  Trying IOS%d... ", ordered[i]);
        VIDEO_WaitVSync();
        if (IOS_ReloadIOS(ordered[i]) >= 0) {
            printf("OK\n");
            loaded = ordered[i];
            break;
        }
        printf("failed\n");
    }
    WPAD_Init();
    return loaded;
}

// -----------------------------------------------------------------------
// Detect Shop Channel title ID
// -----------------------------------------------------------------------
static uint64_t detect_title_id(void) {
    uint64_t candidates[] = {
        SHOP_TITLE_USA, SHOP_TITLE_EUR, SHOP_TITLE_JPN,
        SHOP_TITLE_VWII_USA, SHOP_TITLE_VWII_EUR, SHOP_TITLE_VWII_JPN, 0
    };
    const char *names[] = { "USA", "EUR", "JPN", "vWii USA", "vWii EUR", "vWii JPN" };
    for (int i = 0; candidates[i]; i++) {
        u32 sz = 0;
        if (ES_GetStoredTMDSize(candidates[i], &sz) >= 0 && sz > 0) {
            printf("  Region: %s\n", names[i]);
            return candidates[i];
        }
    }
    return 0;
}

// -----------------------------------------------------------------------
// Read one content from NAND
// -----------------------------------------------------------------------
static uint8_t *read_content(uint64_t title_id, tikview *views, u32 num_views,
                               tmd_content *c, uint32_t *out_size) {
    uint32_t raw_size = (uint32_t)c->size;
    uint8_t *buf = (uint8_t *)memalign(32, (raw_size + 31) & ~31);
    if (!buf) return NULL;

    int fd = ES_OpenTitleContent(title_id, views, c->index);
    if (fd < 0) {
        printf("    [!] ES_OpenTitleContent(index=%d): %d%s\n",
               c->index, fd, fd == -1026 ? " (IOS needs ES patches)" : "");
        free(buf);
        return NULL;
    }
    int ret = ES_ReadContent(fd, buf, raw_size);
    ES_CloseContent(fd);
    if (ret < 0) {
        printf("    [!] ES_ReadContent: %d\n", ret);
        free(buf);
        return NULL;
    }
    *out_size = raw_size;
    return buf;
}

// -----------------------------------------------------------------------
// Backup — save TMD + all raw contents to sd:/shop_backup/
// Skips silently if backup already exists so repeat runs are fast.
// -----------------------------------------------------------------------
static int backup_title(uint64_t title_id,
                         signed_blob *tmd_buf, u32 tmd_size,
                         tikview *views, u32 num_views) {
    // Check if backup already exists
    char tmd_path[128];
    snprintf(tmd_path, sizeof(tmd_path), "%s/title.tmd", BACKUP_DIR);
    struct stat st;
    if (stat(tmd_path, &st) == 0) {
        printf("  Backup already exists, skipping.\n");
        return 0;
    }

    mkdir(BACKUP_DIR, 0777);

    // Save TMD
    FILE *f = fopen(tmd_path, "wb");
    if (!f) { printf("  [!] Cannot write TMD backup\n"); return -1; }
    fwrite(tmd_buf, 1, tmd_size, f);
    fclose(f);
    printf("  TMD saved\n");

    // Save ticket views
    char tik_path[128];
    snprintf(tik_path, sizeof(tik_path), "%s/ticket.bin", BACKUP_DIR);
    f = fopen(tik_path, "wb");
    if (!f) { printf("  [!] Cannot write ticket backup\n"); return -1; }
    fwrite(views, sizeof(tikview), num_views, f);
    fclose(f);
    printf("  Ticket saved\n");

    // Save each content
    tmd *t = SIGNATURE_PAYLOAD(tmd_buf);
    for (int i = 0; i < t->num_contents; i++) {
        tmd_content *c = &t->contents[i];
        printf("  Content %d/%d (index=%d, %lu KB)... ",
               i + 1, t->num_contents, c->index,
               (unsigned long)(c->size / 1024));
        VIDEO_WaitVSync();

        uint32_t size = 0;
        uint8_t *data = read_content(title_id, views, num_views, c, &size);
        if (!data) { printf("read failed\n"); return -1; }

        char cpath[128];
        snprintf(cpath, sizeof(cpath), "%s/%08X.app", BACKUP_DIR, c->cid);
        f = fopen(cpath, "wb");
        if (!f) {
            printf("write failed\n");
            free(data);
            return -1;
        }
        fwrite(data, 1, size, f);
        fclose(f);
        free(data);
        printf("OK\n");
    }

    printf("  Backup complete -> %s\n", BACKUP_DIR);
    return 0;
}

// -----------------------------------------------------------------------
// Load replacement DOL from SD
// -----------------------------------------------------------------------
static uint8_t *load_replacement_dol(uint32_t *out_size) {
    FILE *f = fopen(REPLACE_DOL, "rb");
    if (!f) { printf("  [!] Cannot open %s\n", REPLACE_DOL); return NULL; }

    fseek(f, 0, SEEK_END);
    uint32_t size = (uint32_t)ftell(f);
    fseek(f, 0, SEEK_SET);

    uint32_t aligned = (size + 31) & ~31;
    uint8_t *buf = (uint8_t *)memalign(32, aligned);
    if (!buf) { fclose(f); printf("  [!] Out of memory\n"); return NULL; }
    memset(buf, 0, aligned);
    fread(buf, 1, size, f);
    fclose(f);

    // Basic DOL sanity check — first text section offset must be >= 0x100
    uint32_t first_off = ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) |
                         ((uint32_t)buf[2] << 8)  |  (uint32_t)buf[3];
    if (first_off < 0x100 || first_off > 0x10000) {
        printf("  [!] %s is not a valid DOL\n", REPLACE_DOL);
        free(buf);
        return NULL;
    }

    printf("  DOL: %lu KB\n", (unsigned long)(size / 1024));
    *out_size = aligned;
    return buf;
}

// -----------------------------------------------------------------------
// Reinstall title — all contents kept, boot content replaced with new DOL
// -----------------------------------------------------------------------
static int reinstall_title(uint64_t title_id,
                            signed_blob *tmd_buf, u32 tmd_size,
                            tikview *views, u32 num_views,
                            int boot_index,
                            uint8_t *new_boot, uint32_t new_boot_size) {
    tmd *t = SIGNATURE_PAYLOAD(tmd_buf);

    // Update boot content size + clear hash (fakesigning cIOS skips check)
    for (int i = 0; i < t->num_contents; i++) {
        if (t->contents[i].index == boot_index) {
            t->contents[i].size = new_boot_size;
            memset(t->contents[i].hash, 0, 20);
            break;
        }
    }

    // Dummy cert chain — fakesigning cIOS ignores signatures
    signed_blob *cert_buf = (signed_blob *)memalign(32, 0x400);
    if (!cert_buf) return -1;
    memset(cert_buf, 0, 0x400);

    printf("  ES_AddTitleStart...\n");
    VIDEO_WaitVSync();
    int ret = ES_AddTitleStart(tmd_buf, tmd_size, cert_buf, 0x400,
                               (signed_blob *)views,
                               sizeof(tikview) * num_views);
    free(cert_buf);
    if (ret < 0) {
        printf("  [!] ES_AddTitleStart: %d\n", ret);
        return ret;
    }

    for (int i = 0; i < t->num_contents; i++) {
        tmd_content *c = &t->contents[i];
        printf("  Content %d/%d (index=%d)... ",
               i + 1, t->num_contents, c->index);
        VIDEO_WaitVSync();

        uint8_t *data;
        uint32_t data_size;
        int      free_data = 0;

        if (c->index == boot_index) {
            data      = new_boot;
            data_size = new_boot_size;
        } else {
            data = read_content(title_id, views, num_views, c, &data_size);
            if (!data) {
                printf("read failed\n");
                ES_AddTitleCancel();
                return -2;
            }
            free_data = 1;
        }

        int cfd = ES_AddContentStart(title_id, c->cid);
        if (cfd < 0) {
            printf("ES_AddContentStart: %d\n", cfd);
            if (free_data) free(data);
            ES_AddTitleCancel();
            return cfd;
        }

        ret = ES_AddContentData(cfd, data, data_size);
        if (ret < 0) {
            printf("ES_AddContentData: %d\n", ret);
            if (free_data) free(data);
            ES_AddTitleCancel();
            return ret;
        }

        ret = ES_AddContentFinish(cfd);
        if (free_data) free(data);
        if (ret < 0) {
            printf("ES_AddContentFinish: %d\n", ret);
            ES_AddTitleCancel();
            return ret;
        }
        printf("OK\n");
    }

    ret = ES_AddTitleFinish();
    if (ret < 0) { printf("  [!] ES_AddTitleFinish: %d\n", ret); return ret; }
    return 0;
}

// -----------------------------------------------------------------------
// Main
// -----------------------------------------------------------------------
int main(int argc, char *argv[]) {
    (void)argc; (void)argv;

    init_video();
    WPAD_Init();

    printf("\n");
    printf("  Wii Shop Replacer\n");
    printf("  -----------------\n\n");

    // SD card
    if (!fatInitDefault()) {
        printf("  [!] SD card init failed\n");
        goto wait_exit;
    }
    printf("  SD OK\n\n");

    // 0. cIOS
    printf("[0/5] Loading cIOS...\n");
    int cios = autodetect_and_load_cios();
    if (cios < 0) {
        printf("  [!] No usable cIOS found\n");
        printf("      Install d2x cIOS (slots 249/250) and try again\n");
        goto wait_exit;
    }
    printf("  Running on IOS%d\n\n", cios);

    // 1. Detect Shop Channel
    printf("[1/5] Detecting Shop Channel...\n");
    uint64_t title_id = detect_title_id();
    if (!title_id) {
        printf("  [!] Shop Channel not found\n");
        goto wait_exit;
    }
    printf("  Title ID: %016llX\n\n", (unsigned long long)title_id);

    // 2. Read TMD + ticket views
    printf("[2/5] Reading title metadata...\n");
    u32 tmd_size = 0;
    ES_GetStoredTMDSize(title_id, &tmd_size);
    signed_blob *tmd_buf = (signed_blob *)memalign(32, tmd_size);
    if (!tmd_buf) { printf("  [!] Out of memory\n"); goto wait_exit; }
    ES_GetStoredTMD(title_id, tmd_buf, tmd_size);

    tmd *t = SIGNATURE_PAYLOAD(tmd_buf);
    printf("  %d content(s)\n", t->num_contents);

    // Find boot content (lowest index)
    tmd_content *boot = &t->contents[0];
    for (int i = 1; i < t->num_contents; i++)
        if (t->contents[i].index < boot->index)
            boot = &t->contents[i];
    printf("  Boot content: index=%d  %lu KB\n\n",
           boot->index, (unsigned long)(boot->size / 1024));

    u32 num_views = 0;
    ES_GetNumTicketViews(title_id, &num_views);
    tikview *views = (tikview *)memalign(32, sizeof(tikview) * num_views);
    if (!views) { free(tmd_buf); goto wait_exit; }
    ES_GetTicketViews(title_id, views, num_views);

    // 3. Backup original title to SD
    printf("[3/5] Backing up original Shop Channel...\n");
    if (backup_title(title_id, tmd_buf, tmd_size, views, num_views) < 0) {
        printf("  [!] Backup failed — aborting for safety\n");
        printf("      Check your SD card has enough space\n");
        free(tmd_buf); free(views);
        goto wait_exit;
    }
    printf("\n");

    // 4. Load replacement DOL from SD
    printf("[4/5] Loading replacement DOL...\n");
    uint32_t dol_size = 0;
    uint8_t *dol_buf  = load_replacement_dol(&dol_size);
    if (!dol_buf) {
        printf("  Put your homebrew DOL at: %s\n", REPLACE_DOL);
        free(tmd_buf); free(views);
        goto wait_exit;
    }
    printf("\n");

    // 5. Reinstall with replacement boot content
    printf("[5/5] Reinstalling...\n");
    int ret = reinstall_title(title_id, tmd_buf, tmd_size,
                               views, num_views,
                               boot->index, dol_buf, dol_size);
    free(tmd_buf);
    free(views);
    free(dol_buf);

    if (ret < 0) {
        printf("\n  [!] Failed (ES %d)\n", ret);
        printf("  Your original is safe in %s\n", BACKUP_DIR);
        goto wait_exit;
    }

    printf("\n  Done!\n");
    printf("  Shop Channel icon is unchanged on the Wii menu.\n");
    printf("  Launching it will now run your app.\n");
    printf("  Original backed up to: %s\n", BACKUP_DIR);

wait_exit:
    printf("\n  Press HOME to exit.\n");
    while (1) {
        WPAD_ScanPads();
        if (WPAD_ButtonsDown(0) & WPAD_BUTTON_HOME) break;
        VIDEO_WaitVSync();
    }
    return 0;
}
