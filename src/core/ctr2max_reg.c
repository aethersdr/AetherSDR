#include "ctr2max_reg.h"

#include <ctype.h>
#include <string.h>

#include "lfs.h"

#ifndef LFS_READONLY
#error "ctr2max_reg.c must be built with LFS_READONLY"
#endif

#define CACHE 256

typedef struct {
    ctr2max_reg_read_fn read;
    void *ctx;
} source_t;

static int bd_read(const struct lfs_config *c, lfs_block_t block, lfs_off_t off, void *buffer,
                   lfs_size_t size)
{
    const source_t *s = c->context;
    return s->read(s->ctx, block * c->block_size + off, buffer, size) == 0 ? 0 : LFS_ERR_IO;
}

static int bd_sync(const struct lfs_config *c)
{
    (void)c;
    return 0;
}

static int name_is_reg(const char *name)
{
    static const char want[] = "reg.txt";
    for (size_t i = 0; i < sizeof want; ++i) {
        if (tolower((unsigned char)name[i]) != want[i])
            return 0;
    }
    return 1;
}

ctr2max_reg_t ctr2max_registration(ctr2max_reg_read_fn read, void *ctx, uint32_t size,
                                   char *call, size_t call_size)
{
    static uint8_t read_buf[CACHE], prog_buf[CACHE], look_buf[16], file_buf[CACHE];
    source_t src = {read, ctx};
    struct lfs_config cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.context = &src;
    cfg.read = bd_read;
    cfg.sync = bd_sync;
    cfg.read_size = 16;
    cfg.prog_size = 16;
    cfg.block_size = CTR2MAX_REG_BLOCK;
    cfg.block_count = size / CTR2MAX_REG_BLOCK;
    cfg.block_cycles = 500;
    cfg.cache_size = CACHE;
    cfg.lookahead_size = sizeof look_buf;
    cfg.read_buffer = read_buf;
    cfg.prog_buffer = prog_buf;
    cfg.lookahead_buffer = look_buf;

    if (call_size)
        call[0] = '\0';
    static lfs_t lfs;   // static: kept off the caller's (small) stack
    if (lfs_mount(&lfs, &cfg) != 0)
        return CTR2MAX_REG_UNREADABLE;

    ctr2max_reg_t result = CTR2MAX_NOT_REGISTERED;
    static char path[LFS_NAME_MAX + 2];
    path[0] = '/';
    path[1] = '\0';
    static lfs_dir_t dir;
    if (lfs_dir_open(&lfs, &dir, "/") == 0) {
        struct lfs_info info;
        while (lfs_dir_read(&lfs, &dir, &info) > 0) {
            if (info.type == LFS_TYPE_REG && name_is_reg(info.name)) {
                memcpy(path + 1, info.name, strlen(info.name) + 1);   // at most LFS_NAME_MAX
                // A call in plain text, as before v1.02.00m, is not taken.
                result = info.size > CTR2MAX_REG_MIN_SIZE ? CTR2MAX_REGISTERED
                                                          : CTR2MAX_NOT_REGISTERED;
                break;
            }
        }
        lfs_dir_close(&lfs, &dir);
    } else {
        result = CTR2MAX_REG_UNREADABLE;
    }

    if (result == CTR2MAX_REGISTERED && call_size > 1) {
        struct lfs_file_config fcfg;
        memset(&fcfg, 0, sizeof fcfg);
        fcfg.buffer = file_buf;
        static lfs_file_t file;
        if (lfs_file_opencfg(&lfs, &file, path, LFS_O_RDONLY, &fcfg) == 0) {
            const lfs_ssize_t n = lfs_file_read(&lfs, &file, call, (lfs_size_t)(call_size - 1));
            lfs_file_close(&lfs, &file);
            size_t len = n > 0 ? (size_t)n : 0;
            call[len] = '\0';
            // Trim, and keep only the first line.
            size_t start = 0;
            while (start < len && isspace((unsigned char)call[start]))
                ++start;
            size_t end = start;
            while (end < len && call[end] != '\r' && call[end] != '\n')
                ++end;
            while (end > start && isspace((unsigned char)call[end - 1]))
                --end;
            memmove(call, call + start, end - start);
            call[end - start] = '\0';
        }
    }
    lfs_unmount(&lfs);
    return result;
}
