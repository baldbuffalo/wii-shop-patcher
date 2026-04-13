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

static const int PREFERRED_CIOS[] = { 249, 250, 248, 247, 246, 58, 236, -1 };

// -----------------------------------------------------------------------
// Nintendo LZ77 (type 0x10) decompress
// Returns heap-allocated decompressed buffer, sets *out_len.
// Returns NULL if data is not LZ77 or decompression fails.
// -----------------------------------------------------------------------
static uint8_t *lz77_decompress(const uint8_t *src, uint32_t src_len,
                                uint32_t *out_len) {
    if (src_len < 4 || src[0] != 0x10) return NULL;

    uint32_t dec_size = (uint32_t)src[1]
                      | ((uint32_t)src[2] << 8)
                      | ((uint32_t)src[3] << 16);
    if (dec_size == 0) return NULL;

    uint8_t *dst = (uint8_t *)malloc(dec_size);
    if (!dst) return NULL;

    uint32_t si = 4, di = 0;
    while (di < dec_size && si < src_len) {
        uint8_t flags = src[si++];
        for (int bit = 7; bit >= 0 && di < dec_size && si < src_len; bit--) {
            if (flags & (1 << bit)) {
                if (si + 1 >= src_len) break;
                uint8_t b0 = src[si++];
                uint8_t b1 = src[si++];
                uint32_t len  = (b0 >> 4) + 3;
                uint32_t disp = (((uint32_t)(b0 & 0xF) << 8) | b1) + 1;
                if (disp > di) { free(dst); return NULL; } // corrupt
                uint32_t pos  = di - disp;
                for (uint32_t k = 0; k < len && di < dec_size; k++)
                    dst[di++] = dst[pos++];
            } else {
                dst[di++] = src[si++];
            }
        }
    }

    *out_len = dec_size;
    return dst;
}

// -----------------------------------------------------------------------
// Nintendo LZ77 (type 0x10) compress
// Uses a 3-byte hash table for O(n) average performance — fast enough on
// the Wii's 729 MHz CPU for channel-sized buffers (~1-2 MB decompressed).
// -----------------------------------------------------------------------
#define LZ77_HASH_BITS  16
#define LZ77_HASH_SIZE  (1 << LZ77_HASH_BITS)
#define LZ77_HASH_MASK  (LZ77_HASH_SIZE - 1)
#define LZ77_WIN        4096
#define LZ77_MAXLEN     18
#define LZ77_MINLEN     3

static inline uint32_t lz77_hash3(const uint8_t *p) {
    return (((uint32_t)p[0] * 2654435761u) ^
            ((uint32_t)p[1] * 40503u) ^
            ((uint32_t)p[2])) & LZ77_HASH_MASK;
}

static uint8_t *lz77_compress(const uint8_t *src, uint32_t src_len,
                               uint32_t *out_len) {
    // Worst case output: header + ceil(src_len/8) flag bytes + src_len literals
    uint32_t max_out = 4 + src_len + (src_len / 8) + 4;
    uint8_t *dst = (uint8_t *)malloc(max_out);
    if (!dst) return NULL;

    // Hash table: maps hash -> most recent source position with that hash
    int32_t *htab = (int32_t *)malloc(LZ77_HASH_SIZE * sizeof(int32_t));
    if (!htab) { free(dst); return NULL; }
    memset(htab, 0xFF, LZ77_HASH_SIZE * sizeof(int32_t)); // -1 = empty

    dst[0] = 0x10;
    dst[1] = (uint8_t)(src_len);
    dst[2] = (uint8_t)(src_len >> 8);
    dst[3] = (uint8_t)(src_len >> 16);

    uint32_t si = 0, di = 4;

    while (si < src_len) {
        uint32_t flag_pos = di++;
        uint8_t  flags    = 0;

        for (int bit = 7; bit >= 0 && si < src_len; bit--) {
            uint32_t best_len = 0, best_disp = 0;

            if (si + LZ77_MINLEN <= src_len) {
                uint32_t h = lz77_hash3(&src[si]);
                int32_t  prev = htab[h];

                // Walk chain (one step — good balance of speed vs ratio)
                if (prev >= 0 && (uint32_t)prev < si) {
                    uint32_t disp = si - (uint32_t)prev;
                    if (disp <= LZ77_WIN) {
                        uint32_t max_len = src_len - si;
                        if (max_len > LZ77_MAXLEN) max_len = LZ77_MAXLEN;
                        uint32_t len = 0;
                        const uint8_t *a = src + prev;
                        const uint8_t *b = src + si;
                        while (len < max_len && a[len] == b[len]) len++;
                        if (len >= LZ77_MINLEN) {
                            best_len  = len;
                            best_disp = disp;
                        }
                    }
                }
                // Also do a short brute-force scan for better matches
                uint32_t scan_start = (si >= LZ77_WIN) ? si - LZ77_WIN : 0;
                for (uint32_t p = scan_start; p < si; p++) {
                    uint32_t max_len = src_len - si;
                    if (max_len > LZ77_MAXLEN) max_len = LZ77_MAXLEN;
                    uint32_t len = 0;
                    while (len < max_len && src[p + len] == src[si + len]) len++;
                    if (len > best_len) {
                        best_len  = len;
                        best_disp = si - p;
                        if (best_len == LZ77_MAXLEN) break;
                    }
                }

                htab[h] = (int32_t)si;
            }

            if (best_len >= LZ77_MINLEN) {
                flags |= (1 << bit);
                uint32_t d = best_disp - 1;
                dst[di++] = (uint8_t)(((best_len - 3) << 4) | ((d >> 8) & 0xF));
                dst[di++] = (uint8_t)(d & 0xFF);
                // Update hash for skipped bytes
                for (uint32_t k = 1; k < best_len && (si + k + 2) < src_len; k++)
                    htab[lz77_hash3(&src[si + k])] = (int32_t)(si + k);
                si += best_len;
            } else {
                if (si + 2 < src_len)
                    htab[lz77_hash3(&src[si])] = (int32_t)si;
                dst[di++] = src[si++];
            }
        }
        dst[flag_pos] = flags;
    }

    free(htab);
    *out_len = di;
    return dst;
}

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
// WPAD is shut down before IOS_ReloadIOS (it tears down the BT kernel)
// and reinitialised after so the HOME button always works.
// -----------------------------------------------------------------------
static int autodetect_and_load_cios(void) {
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

    int ios_slots[256], ios_count = 0;
    for (u32 i = 0; i < num_titles; i++) {
        uint32_t hi = (uint32_t)(title_list[i] >> 32);
        uint32_t lo = (uint32_t)(title_list[i] & 0xFFFFFFFF);
        if (hi == 0x00000001 && lo >= 3 && lo <= 255)
            ios_slots[ios_count++] = (int)lo;
    }
    free(title_list);

    printf("  Found %d IOS title(s) installed\n", ios_count);
    if (ios_count == 0) return -1;

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

    WPAD_Shutdown();

    int loaded = -1;
    for (int i = 0; i < ordered_count; i++) {
        int slot = ordered[i];
        printf("  Trying IOS%d... ", slot);
        VIDEO_WaitVSync();
        if (IOS_ReloadIOS(slot) >= 0) {
            printf("OK\n");
            loaded = slot;
            break;
        }
        printf("failed\n");
    }

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
// Read content from NAND, transparently decompress if LZ77
// -----------------------------------------------------------------------
static uint8_t *read_nand_content(uint64_t title_id, uint32_t *out_len,
                                  int *was_compressed) {
    *was_compressed = 0;

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

    // Boot content = lowest index value in the TMD contents array
    tmd_content *boot_content = &t->contents[0];
    for (int i = 1; i < t->num_contents; i++) {
        if (t->contents[i].index < boot_content->index)
            boot_content = &t->contents[i];
    }
    printf("  Boot content: index %d, %lu KB\n",
           boot_content->index, (unsigned long)(boot_content->size / 1024));

    uint32_t raw_size = (uint32_t)boot_content->size;
    uint8_t *raw_buf  = (uint8_t *)memalign(32, raw_size);
    if (!raw_buf) {
        printf("  [!] Out of memory (content).\n");
        free(tmd_buf);
        return NULL;
    }

    u32 num_views = 0;
    if (ES_GetNumTicketViews(title_id, &num_views) < 0 || num_views == 0) {
        printf("  [!] ES_GetNumTicketViews failed.\n");
        free(tmd_buf); free(raw_buf);
        return NULL;
    }

    tikview *views = (tikview *)memalign(32, sizeof(tikview) * num_views);
    if (!views) {
        printf("  [!] Out of memory (ticket views).\n");
        free(tmd_buf); free(raw_buf);
        return NULL;
    }
    if (ES_GetTicketViews(title_id, views, num_views) < 0) {
        printf("  [!] ES_GetTicketViews failed.\n");
        free(tmd_buf); free(raw_buf); free(views);
        return NULL;
    }

    int fd = ES_OpenTitleContent(title_id, views, boot_content->index);
    if (fd < 0) {
        printf("  [!] ES_OpenTitleContent failed: %d\n", fd);
        if (fd == -1026)
            printf("      Loaded IOS lacks ES patches.\n");
        free(tmd_buf); free(raw_buf); free(views);
        return NULL;
    }

    int ret = ES_ReadContent(fd, raw_buf, raw_size);
    ES_CloseContent(fd);
    free(tmd_buf);
    free(views);

    if (ret < 0) {
        printf("  [!] ES_ReadContent failed: %d\n", ret);
        free(raw_buf);
        return NULL;
    }

    printf("  Raw content: %02X %02X %02X %02X (magic)\n",
           raw_buf[0], raw_buf[1], raw_buf[2], raw_buf[3]);

    // Detect and transparently decompress LZ77 (type 0x10)
    if (raw_buf[0] == 0x10) {
        printf("  LZ77 compressed — decompressing...\n");
        uint32_t dec_size = 0;
        uint8_t *dec_buf  = lz77_decompress(raw_buf, raw_size, &dec_size);
        free(raw_buf);
        if (!dec_buf) {
            printf("  [!] LZ77 decompression failed.\n");
            return NULL;
        }
        printf("  Decompressed: %lu KB\n", (unsigned long)(dec_size / 1024));
        *was_compressed = 1;
        *out_len = dec_size;
        return dec_buf;
    }

    printf("  Read %lu KB (uncompressed)\n", (unsigned long)(raw_size / 1024));
    *out_len = raw_size;
    return raw_buf;
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
// If was_compressed, recompress with LZ77 before writing.
// -----------------------------------------------------------------------
static int write_nand_content(uint64_t title_id,
                               uint8_t *content_buf, uint32_t content_size,
                               int was_compressed) {
    uint8_t *write_buf  = content_buf;
    uint32_t write_size = content_size;
    uint8_t *comp_buf   = NULL;

    if (was_compressed) {
        printf("  Recompressing (LZ77)...\n");
        VIDEO_WaitVSync();
        uint32_t comp_size = 0;
        comp_buf = lz77_compress(content_buf, content_size, &comp_size);
        if (!comp_buf) {
            printf("  [!] LZ77 compression failed.\n");
            return -10;
        }
        printf("  Compressed: %lu KB\n", (unsigned long)(comp_size / 1024));
        write_buf  = comp_buf;
        write_size = comp_size;
    }

    u32 tmd_size = 0;
    ES_GetStoredTMDSize(title_id, &tmd_size);
    signed_blob *tmd_buf = (signed_blob *)memalign(32, tmd_size);
    if (!tmd_buf) { free(comp_buf); return -1; }
    ES_GetStoredTMD(title_id, tmd_buf, tmd_size);

    u32 num_views = 0;
    if (ES_GetNumTicketViews(title_id, &num_views) < 0 || num_views == 0) {
        free(tmd_buf); free(comp_buf);
        return -2;
    }
    tikview *ticket_buf = (tikview *)memalign(32, sizeof(tikview) * num_views);
    if (!ticket_buf) { free(tmd_buf); free(comp_buf); return -2; }
    if (ES_GetTicketViews(title_id, ticket_buf, num_views) < 0) {
        printf("  [!] ES_GetTicketViews failed.\n");
        free(tmd_buf); free(ticket_buf); free(comp_buf);
        return -3;
    }

    signed_blob *cert_buf = (signed_blob *)memalign(32, 0x400);
    if (!cert_buf) { free(tmd_buf); free(ticket_buf); free(comp_buf); return -4; }

    printf("  Starting ES install...\n");
    int ret = ES_AddTitleStart(tmd_buf, tmd_size, cert_buf, 0x400,
                               (signed_blob *)ticket_buf,
                               sizeof(tikview) * num_views);
    free(cert_buf);
    if (ret < 0) {
        printf("  [!] ES_AddTitleStart failed: %d\n", ret);
        free(tmd_buf); free(ticket_buf); free(comp_buf);
        return ret;
    }

    tmd *t = SIGNATURE_PAYLOAD(tmd_buf);
    uint32_t cid = t->contents[0].cid;

    int content_fd = ES_AddContentStart(title_id, cid);
    if (content_fd < 0) {
        printf("  [!] ES_AddContentStart failed: %d\n", content_fd);
        ES_AddTitleCancel();
        free(tmd_buf); free(ticket_buf); free(comp_buf);
        return content_fd;
    }

    ret = ES_AddContentData(content_fd, write_buf, write_size);
    if (ret < 0) {
        printf("  [!] ES_AddContentData failed: %d\n", ret);
        ES_AddTitleCancel();
        free(tmd_buf); free(ticket_buf); free(comp_buf);
        return ret;
    }

    ret = ES_AddContentFinish(content_fd);
    if (ret < 0) {
        printf("  [!] ES_AddContentFinish failed: %d\n", ret);
        ES_AddTitleCancel();
        free(tmd_buf); free(ticket_buf); free(comp_buf);
        return ret;
    }

    ret = ES_AddTitleFinish();
    free(tmd_buf); free(ticket_buf); free(comp_buf);
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

    // 0. Auto-detect and load a suitable cIOS
    printf("[0/4] Detecting cIOS...\n");
    int cios = autodetect_and_load_cios();
    if (cios < 0) {
        printf("  [!] No usable IOS found.\n");
        printf("      Install d2x cIOS (slots 249/250) and try again.\n");
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

    // 2. Read + decompress content from NAND
    printf("[2/4] Reading content from NAND...\n");
    uint32_t content_len  = 0;
    int      was_compressed = 0;
    uint8_t *content_buf  = read_nand_content(title_id, &content_len,
                                               &was_compressed);
    if (!content_buf) {
        printf("  [!] Failed to read NAND content. Aborting.\n");
        goto wait_exit;
    }
    printf("\n");

    // 3. Patch URLs in decompressed buffer
    printf("[3/4] Patching URLs...\n");
    uint32_t hits = patch_buffer(content_buf, content_len);
    if (hits == 0) {
        printf("  [!] No URL matches found.\n");
        printf("      Content format is unrecognised.\n");
        free(content_buf);
        goto wait_exit;
    }
    printf("  Replaced %lu occurrence(s)\n\n", (unsigned long)hits);

    // 4. Recompress (if needed) and write back to NAND
    printf("[4/4] Writing patched content to NAND...\n");
    int ret = write_nand_content(title_id, content_buf, content_len,
                                  was_compressed);
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
