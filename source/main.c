#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include <malloc.h>
#include <network.h>
#include <ogcsys.h>
#include <gccore.h>
#include <wiiuse/wpad.h>
#include <ogc/es.h>
#include <fat.h>

// -----------------------------------------------------------------------
// Config — replace test.net---- with your domain (must stay 12 chars)
// -----------------------------------------------------------------------
#define YOUR_DOMAIN "vimm.net----"

static const char *patches[][2] = {
    { "ecs.shop.wii.com", "ecs." YOUR_DOMAIN },
    { "ias.shop.wii.com", "ias." YOUR_DOMAIN },
    { "ccs.shop.wii.com", "ccs." YOUR_DOMAIN },
    { "nus.shop.wii.com", "nus." YOUR_DOMAIN },
    { "/ccs/download/WiiWare", "/vault/WiiWare       " },
    { "/ccs/download/Wii----", "/vault/Wii-----------" },
    { NULL, NULL }
};

#define SHOP_TITLE_USA      0x0001000248414241ULL
#define SHOP_TITLE_EUR      0x0001000248414245ULL
#define SHOP_TITLE_JPN      0x000100024841424aULL
#define SHOP_TITLE_VWII_USA 0x000100024841424bULL
#define SHOP_TITLE_VWII_EUR 0x000100024841424cULL
#define SHOP_TITLE_VWII_JPN 0x000100024841424dULL

// Known-good cIOS slots tried first, then full scan fills the rest
static const int PREFERRED_CIOS[] = { 249, 250, 248, 247, 246, 58, 236, -1 };

// -----------------------------------------------------------------------
// Structs
// -----------------------------------------------------------------------
typedef struct {
    uint32_t header_size;
    uint16_t wad_type;
    uint16_t wad_version;
    uint32_t cert_chain_size;
    uint32_t reserved;
    uint32_t ticket_size;
    uint32_t tmd_size;
    uint32_t data_size;
    uint32_t footer_size;
} __attribute__((packed)) WadHeader;

typedef struct {
    uint8_t *cert_chain;
    uint32_t cert_chain_size;
    uint8_t *ticket;
    uint32_t ticket_size;
    uint8_t *tmd;
    uint32_t tmd_size;
    uint8_t *content;
    uint32_t content_size;
} WadSections;

// -----------------------------------------------------------------------
// Video init
// -----------------------------------------------------------------------
static GXRModeObj *rmode = NULL;
static void *xfb = NULL;

static void init_video(void) {
    VIDEO_Init();
    rmode = VIDEO_GetPreferredMode(NULL);
    xfb = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
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
// IOS auto-detection
//
// IMPORTANT: IOS_ReloadIOS tears down the entire IOS kernel including the
// Bluetooth stack. WPAD must be shut down before the reload and
// re-initialised after, otherwise the console freezes.
// -----------------------------------------------------------------------
static int autodetect_and_load_cios(void) {
    // --- enumerate installed titles ---
    u32 num_titles = 0;
    if (ES_GetNumTitles(&num_titles) < 0 || num_titles == 0) {
        printf("  [!] ES_GetNumTitles failed.\n");
        return -1;
    }

    uint64_t *title_list = (uint64_t *)memalign(32, sizeof(uint64_t) * num_titles);
    if (!title_list) { printf("  [!] Out of memory.\n"); return -1; }

    if (ES_GetTitles(title_list, num_titles) < 0) {
        printf("  [!] ES_GetTitles failed.\n");
        free(title_list);
        return -1;
    }

    // Collect IOS slots (high word == 0x00000001, slot 3-255)
    int ios_slots[256];
    int ios_count = 0;
    for (u32 i = 0; i < num_titles; i++) {
        uint32_t hi = (uint32_t)(title_list[i] >> 32);
        uint32_t lo = (uint32_t)(title_list[i] & 0xFFFFFFFF);
        if (hi == 0x00000001 && lo >= 3 && lo <= 255)
            ios_slots[ios_count++] = (int)lo;
    }
    free(title_list);

    printf("  Found %d IOS title(s) installed\n", ios_count);
    if (ios_count == 0) return -1;

    // Build ordered candidate list: preferred slots first, then high-to-low
    int ordered[256];
    int ordered_count = 0;
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

    // Try each candidate.
    // Shut down WPAD first — IOS_ReloadIOS resets the BT/IOS kernel and
    // will freeze the console if the Bluetooth stack is still running.
    WPAD_Shutdown();

    int loaded = -1;
    for (int i = 0; i < ordered_count; i++) {
        int slot = ordered[i];
        printf("  Trying IOS%d... ", slot);
        VIDEO_WaitVSync(); // flush printf to framebuffer before reload

        if (IOS_ReloadIOS(slot) >= 0) {
            printf("OK\n");
            loaded = slot;
            break;
        }
        printf("failed\n");
    }

    // Reinitialise WPAD regardless of outcome so the HOME button works
    WPAD_Init();

    return loaded;
}

// -----------------------------------------------------------------------
// Detect installed Shop Channel region
// -----------------------------------------------------------------------
static uint64_t detect_title_id(void) {
    uint64_t candidates[] = {
        SHOP_TITLE_USA, SHOP_TITLE_EUR, SHOP_TITLE_JPN,
        SHOP_TITLE_VWII_USA, SHOP_TITLE_VWII_EUR, SHOP_TITLE_VWII_JPN,
        0
    };
    const char *names[] = { "USA", "EUR", "JPN", "vWii USA", "vWii EUR", "vWii JPN" };

    for (int i = 0; candidates[i]; i++) {
        u32 tmd_size = 0;
        if (ES_GetStoredTMDSize(candidates[i], &tmd_size) >= 0 && tmd_size > 0) {
            printf("  Detected region: %s\n", names[i]);
            return candidates[i];
        }
    }
    return 0;
}

// -----------------------------------------------------------------------
// Read existing Shop Channel content from NAND via ES
// -----------------------------------------------------------------------
static uint8_t *read_nand_content(uint64_t title_id, uint32_t *out_len) {
    u32 tmd_size = 0;
    if (ES_GetStoredTMDSize(title_id, &tmd_size) < 0) {
        printf("  [!] Could not get TMD size.\n");
        return NULL;
    }

    signed_blob *tmd_buf = (signed_blob *)memalign(32, tmd_size);
    if (!tmd_buf) { printf("  [!] Out of memory (TMD).\n"); return NULL; }

    if (ES_GetStoredTMD(title_id, tmd_buf, tmd_size) < 0) {
        printf("  [!] Could not read TMD.\n");
        free(tmd_buf);
        return NULL;
    }

    tmd *t = SIGNATURE_PAYLOAD(tmd_buf);
    printf("  TMD has %d content(s)\n", t->num_contents);

    // Find boot content — lowest TMD index value
    tmd_content *boot_content = &t->contents[0];
    for (int i = 1; i < t->num_contents; i++) {
        if (t->contents[i].index < boot_content->index)
            boot_content = &t->contents[i];
    }
    printf("  Boot content: index %d, %lu KB\n",
           boot_content->index, (unsigned long)(boot_content->size / 1024));

    uint32_t content_size = (uint32_t)boot_content->size;
    uint8_t *content_buf = (uint8_t *)memalign(32, content_size);
    if (!content_buf) {
        printf("  [!] Out of memory (content).\n");
        free(tmd_buf);
        return NULL;
    }

    u32 num_views = 0;
    if (ES_GetNumTicketViews(title_id, &num_views) < 0 || num_views == 0) {
        printf("  [!] ES_GetNumTicketViews failed or no views.\n");
        free(tmd_buf); free(content_buf);
        return NULL;
    }

    tikview *views = (tikview *)memalign(32, sizeof(tikview) * num_views);
    if (!views) {
        printf("  [!] Out of memory (ticket views).\n");
        free(tmd_buf); free(content_buf);
        return NULL;
    }
    if (ES_GetTicketViews(title_id, views, num_views) < 0) {
        printf("  [!] ES_GetTicketViews failed.\n");
        free(tmd_buf); free(content_buf); free(views);
        return NULL;
    }

    int fd = ES_OpenTitleContent(title_id, views, boot_content->index);
    if (fd < 0) {
        printf("  [!] ES_OpenTitleContent failed: %d\n", fd);
        if (fd == -1026)
            printf("      Loaded IOS lacks ES patches. Try a different cIOS.\n");
        free(tmd_buf); free(content_buf); free(views);
        return NULL;
    }

    int ret = ES_ReadContent(fd, content_buf, content_size);
    ES_CloseContent(fd);
    free(tmd_buf);
    free(views);

    if (ret < 0) {
        printf("  [!] ES_ReadContent failed: %d\n", ret);
        free(content_buf);
        return NULL;
    }

    *out_len = content_size;
    return content_buf;
}

// -----------------------------------------------------------------------
// URL patcher
// -----------------------------------------------------------------------
static uint32_t patch_buffer(uint8_t *buf, uint32_t len) {
    uint32_t total = 0;
    for (int p = 0; patches[p][0] != NULL; p++) {
        size_t orig_len = strlen(patches[p][0]);
        size_t repl_len = strlen(patches[p][1]);
        if (repl_len > orig_len) {
            printf("  [!] Patch %d: replacement longer than original, skipping\n", p);
            continue;
        }
        for (uint32_t i = 0; i < len - orig_len; i++) {
            if (memcmp(&buf[i], patches[p][0], orig_len) == 0) {
                memcpy(&buf[i], patches[p][1], repl_len);
                if (repl_len < orig_len)
                    memset(&buf[i + repl_len], 0, orig_len - repl_len);
                total++;
                i += orig_len - 1;
            }
        }
    }
    return total;
}

// -----------------------------------------------------------------------
// Write patched content back to NAND via ES
// -----------------------------------------------------------------------
static int write_nand_content(uint64_t title_id, uint8_t *content_buf,
                              uint32_t content_size) {
    u32 tmd_size = 0;
    ES_GetStoredTMDSize(title_id, &tmd_size);
    signed_blob *tmd_buf = (signed_blob *)memalign(32, tmd_size);
    if (!tmd_buf) return -1;
    ES_GetStoredTMD(title_id, tmd_buf, tmd_size);

    u32 num_views = 0;
    if (ES_GetNumTicketViews(title_id, &num_views) < 0 || num_views == 0) {
        free(tmd_buf);
        return -2;
    }
    tikview *ticket_buf = (tikview *)memalign(32, sizeof(tikview) * num_views);
    if (!ticket_buf) { free(tmd_buf); return -2; }
    if (ES_GetTicketViews(title_id, ticket_buf, num_views) < 0) {
        printf("  [!] ES_GetTicketViews failed.\n");
        free(tmd_buf); free(ticket_buf);
        return -3;
    }

    signed_blob *cert_buf = (signed_blob *)memalign(32, 0x400);
    if (!cert_buf) { free(tmd_buf); free(ticket_buf); return -4; }

    printf("  Starting ES install...\n");
    int ret = ES_AddTitleStart(tmd_buf, tmd_size, cert_buf, 0x400,
                               (signed_blob *)ticket_buf,
                               sizeof(tikview) * num_views);
    free(cert_buf);
    if (ret < 0) {
        printf("  [!] ES_AddTitleStart failed: %d\n", ret);
        free(tmd_buf); free(ticket_buf);
        return ret;
    }

    tmd *t = SIGNATURE_PAYLOAD(tmd_buf);
    uint32_t cid = t->contents[0].cid;

    int content_fd = ES_AddContentStart(title_id, cid);
    if (content_fd < 0) {
        printf("  [!] ES_AddContentStart failed: %d\n", content_fd);
        ES_AddTitleCancel();
        free(tmd_buf); free(ticket_buf);
        return content_fd;
    }

    ret = ES_AddContentData(content_fd, content_buf, content_size);
    if (ret < 0) {
        printf("  [!] ES_AddContentData failed: %d\n", ret);
        ES_AddTitleCancel();
        free(tmd_buf); free(ticket_buf);
        return ret;
    }

    ret = ES_AddContentFinish(content_fd);
    if (ret < 0) {
        printf("  [!] ES_AddContentFinish failed: %d\n", ret);
        ES_AddTitleCancel();
        free(tmd_buf); free(ticket_buf);
        return ret;
    }

    ret = ES_AddTitleFinish();
    free(tmd_buf); free(ticket_buf);
    if (ret < 0) {
        printf("  [!] ES_AddTitleFinish failed: %d\n", ret);
        return ret;
    }

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
    printf("  Wii Shop Patcher\n");
    printf("  ----------------\n\n");

    // 0. Auto-detect and load a suitable IOS.
    //    autodetect_and_load_cios() handles WPAD_Shutdown/WPAD_Init
    //    around the IOS reload internally.
    printf("[0/4] Detecting cIOS...\n");
    int cios = autodetect_and_load_cios();
    if (cios < 0) {
        printf("  [!] No usable IOS found on this console.\n");
        printf("      Install d2x cIOS (slots 249/250) via the\n");
        printf("      d2x cIOS installer and try again.\n");
        goto wait_exit;
    }
    printf("  Running on IOS%d\n\n", cios);

    // 1. Detect region
    printf("[1/4] Detecting Shop Channel...\n");
    uint64_t title_id = detect_title_id();
    if (!title_id) {
        printf("  [!] Shop Channel not found on this Wii.\n");
        goto wait_exit;
    }
    printf("  Title ID: %016llX\n\n", (unsigned long long)title_id);

    // 2. Read content from NAND
    printf("[2/4] Reading content from NAND...\n");
    uint32_t content_len = 0;
    uint8_t *content_buf = read_nand_content(title_id, &content_len);
    if (!content_buf) {
        printf("  [!] Failed to read NAND content. Aborting.\n");
        goto wait_exit;
    }
    printf("  Read %lu KB\n\n", (unsigned long)(content_len / 1024));

    // 3. Patch URLs
    printf("[3/4] Patching URLs...\n");
    uint32_t hits = patch_buffer(content_buf, content_len);
    if (hits == 0) {
        printf("  [!] No URL matches found.\n");
        printf("      Already patched, or unexpected content version.\n");
        free(content_buf);
        goto wait_exit;
    }
    printf("  Replaced %lu occurrence(s)\n\n", (unsigned long)hits);

    // 4. Write back to NAND
    printf("[4/4] Writing patched content to NAND...\n");
    int ret = write_nand_content(title_id, content_buf, content_len);
    free(content_buf);
    if (ret < 0) {
        printf("  [!] Write failed (ES error %d). Aborting.\n", ret);
        goto wait_exit;
    }

    printf("\n  Done!\n");
    printf("  Shop Channel now points to: %s\n", YOUR_DOMAIN);
    printf("  You can delete this app from your SD card.\n");

wait_exit:
    printf("\n  Press HOME or RESET to exit.\n");
    while (1) {
        WPAD_ScanPads();
        if (WPAD_ButtonsDown(0) & WPAD_BUTTON_HOME) break;
        VIDEO_WaitVSync();
    }
    return 0;
}
