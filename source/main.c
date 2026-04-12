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
#include <es.h>
#include <fat.h>

// -----------------------------------------------------------------------
// Config — replace with your domain
// IMPORTANT: each replacement must be the exact same byte length
// as the original. "shop.wii.com" = 12 chars, so your domain
// must also be 12 chars. Pad with dashes if needed e.g. "mysite-.net"
// -----------------------------------------------------------------------
#define YOUR_DOMAIN "site.net----"  // must stay 12 chars — replace site.net with your domain and adjust dashes

static const char *patches[][2] = {
    { "ecs.shop.wii.com", "ecs." YOUR_DOMAIN },
    { "ias.shop.wii.com", "ias." YOUR_DOMAIN },
    { "ccs.shop.wii.com", "ccs." YOUR_DOMAIN },
    { "nus.shop.wii.com", "nus." YOUR_DOMAIN },
    { NULL, NULL }
};

// Shop Channel title IDs by region
#define SHOP_TITLE_USA 0x0001000248414241ULL
#define SHOP_TITLE_EUR 0x0001000248414245ULL
#define SHOP_TITLE_JPN 0x000100024841424aULL

// -----------------------------------------------------------------------
// Structs
// -----------------------------------------------------------------------
typedef struct {
    uint8_t  *data;
    uint32_t  len;
} Buffer;

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
// Detect installed region — tries USA, EUR, JPN in order
// -----------------------------------------------------------------------
static uint64_t detect_title_id(void) {
    uint64_t candidates[] = {
        SHOP_TITLE_USA,
        SHOP_TITLE_EUR,
        SHOP_TITLE_JPN,
        0
    };
    const char *names[] = { "USA", "EUR", "JPN" };

    for (int i = 0; candidates[i]; i++) {
        uint32_t num_contents = 0;
        if (ES_GetNumStoredTMDContents(candidates[i], 0, &num_contents) >= 0
            && num_contents > 0) {
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
    // Get TMD to find content records
    uint32_t tmd_size = 0;
    if (ES_GetStoredTMDSize(title_id, 0, &tmd_size) < 0) {
        printf("  [!] Could not get TMD size.\n");
        return NULL;
    }

    signed_blob *tmd_buf = (signed_blob *)memalign(32, tmd_size);
    if (!tmd_buf) { printf("  [!] Out of memory.\n"); return NULL; }

    if (ES_GetStoredTMD(title_id, 0, tmd_buf, tmd_size) < 0) {
        printf("  [!] Could not read TMD.\n");
        free(tmd_buf);
        return NULL;
    }

    // Get content count
    tmd *t = SIGNATURE_PAYLOAD(tmd_buf);
    uint16_t num_contents = t->num_contents;
    printf("  TMD has %d content(s)\n", num_contents);

    // Read first content (the main app — index 0)
    tmd_content *content_rec = &t->contents[0];
    uint32_t content_size = (uint32_t)content_rec->size;
    uint32_t cid = content_rec->cid;

    uint8_t *content_buf = (uint8_t *)memalign(32, content_size);
    if (!content_buf) {
        printf("  [!] Out of memory for content.\n");
        free(tmd_buf);
        return NULL;
    }

    int fd = ES_OpenContent(title_id, cid);
    if (fd < 0) {
        printf("  [!] ES_OpenContent failed: %d\n", fd);
        free(tmd_buf);
        free(content_buf);
        return NULL;
    }

    int ret = ES_ReadContent(fd, content_buf, content_size);
    ES_CloseContent(fd);
    free(tmd_buf);

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
    // Need fresh TMD and ticket to reinstall
    uint32_t tmd_size = 0;
    ES_GetStoredTMDSize(title_id, 0, &tmd_size);
    signed_blob *tmd_buf = (signed_blob *)memalign(32, tmd_size);
    if (!tmd_buf) return -1;
    ES_GetStoredTMD(title_id, 0, tmd_buf, tmd_size);

    // Get ticket
    uint32_t ticket_size = 0;
    ES_GetTicketViewSize(title_id, &ticket_size);
    // Full ticket is 0x2A4 bytes
    signed_blob *ticket_buf = (signed_blob *)memalign(32, 0x2A4);
    if (!ticket_buf) { free(tmd_buf); return -2; }
    ES_GetTicketViews(title_id, (tikview *)ticket_buf, 1);

    // Get certs
    uint32_t cert_size = 0x400; // standard cert chain size
    signed_blob *cert_buf = (signed_blob *)memalign(32, cert_size);
    if (!cert_buf) { free(tmd_buf); free(ticket_buf); return -3; }

    printf("  Starting ES install...\n");
    int ret = ES_AddTitleStart(tmd_buf, tmd_size, cert_buf, cert_size,
                               ticket_buf, 0x2A4);
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

    // 1. Detect region
    printf("[1/3] Detecting Shop Channel...\n");
    uint64_t title_id = detect_title_id();
    if (!title_id) {
        printf("  [!] Shop Channel not found on this Wii.\n");
        goto wait_exit;
    }
    printf("  Title ID: %016llX\n\n", (unsigned long long)title_id);

    // 2. Read content from NAND
    printf("[2/3] Reading content from NAND...\n");
    uint32_t content_len = 0;
    uint8_t *content_buf = read_nand_content(title_id, &content_len);
    if (!content_buf) {
        printf("  [!] Failed to read NAND content. Aborting.\n");
        goto wait_exit;
    }
    printf("  Read %lu KB\n\n", (unsigned long)(content_len / 1024));

    // 3. Patch URLs
    printf("[3/3] Patching URLs...\n");
    uint32_t hits = patch_buffer(content_buf, content_len);
    if (hits == 0) {
        printf("  [!] No URL matches found.\n");
        printf("      Already patched, or unexpected content version.\n");
        free(content_buf);
        goto wait_exit;
    }
    printf("  Replaced %lu occurrence(s)\n\n", (unsigned long)hits);

    // 4. Write back to NAND
    printf("[4/3] Writing patched content to NAND...\n");
    int ret = write_nand_content(title_id, content_buf, content_len);
    free(content_buf);
    if (ret < 0) {
        printf("  [!] Write failed (ES error %d). Aborting.\n", ret);
        goto wait_exit;
    }

    printf("\n  Done!\n");
    printf("  Shop Channel now points to: site.net\n");
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
