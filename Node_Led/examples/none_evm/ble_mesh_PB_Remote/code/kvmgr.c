/*
 * Copyright (C) 2015-2017 Alibaba Group Holding Limited
 */

#include <errno.h>
#include <string.h>

#include "kvmgr.h"

#include "co_printf.h"
#include "co_log.h"

#include "driver_flash.h"
#include "driver_system.h"
#include "os_mem.h"

#define aos_malloc  os_malloc
#define aos_free    os_free

/* Key-value function return code description */
typedef enum
{
    RES_OK = 0,                   /* Successed */
    RES_CONT = -1,           /* Loop continued */
    RES_NO_SPACE = -2,       /* The space is out of range */
    RES_INVALID_PARAM = -3,  /* The parameter is invalid */
    RES_MALLOC_FAILED = -4,  /* Error related to malloc */
    RES_ITEM_NOT_FOUND = -5, /* Could not find the key-value item */
    RES_FLASH_READ_ERR = -6,    /* The flash read operation failed */
    RES_FLASH_WRITE_ERR = -7,   /* The flash write operation failed */
    RES_FLASH_EARSE_ERR = -8    /* The flash earse operation failed */
} result_e;

/* Defination of block information */
#define BLK_BITS 12                          /* The number of bits in block size */
#define BLK_SIZE (1 << BLK_BITS)             /* Block size, current is 4k bytes */
#define BLK_NUMS (KV_TOTAL_SIZE >> BLK_BITS) /* The number of blocks, must be bigger than KV_GC_RESERVED */
#define BLK_OFF_MASK ~(BLK_SIZE - 1)         /* The mask of block offset in key-value store */
#define BLK_STATE_USED 0xCC                  /* Block state: USED --> block is inused and without dirty data */
#define BLK_STATE_CLEAN 0xEE                 /* Block state: CLEAN --> block is clean, ready for used */
#define BLK_STATE_DIRTY 0x44                 /* Block state: DIRTY --> block is inused and with dirty data */
#define BLK_HEADER_SIZE 4                    /* The block header size 4bytes */

#define INVALID_BLK_STATE(state)     \
    (((state) != BLK_STATE_USED) &&  \
     ((state) != BLK_STATE_CLEAN) && \
     ((state) != BLK_STATE_DIRTY))

/* Defination of key-value item information */
#define ITEM_HEADER_SIZE 8     /* The key-value item header size 8bytes */
#define ITEM_STATE_NORMAL 0xEE /* Key-value item state: NORMAL --> the key-value item is valid */
#define ITEM_STATE_DELETE 0    /* Key-value item state: DELETE --> the key-value item is deleted */
#define ITEM_MAX_KEY_LEN 64     //128   /* The max key length for key-value item */
#define ITEM_MAX_VAL_LEN 128   //768   //512  localtime       /* The max value length for key-value item */
#define ITEM_MAX_LEN (ITEM_HEADER_SIZE + ITEM_MAX_KEY_LEN + ITEM_MAX_VAL_LEN)

/* Defination of key-value store information */
#define KV_STATE_OFF 1                      /* The offset of block/item state in header structure */
#define KV_ALIGN_MASK ~(sizeof(void *) - 1) /* The mask of key-value store alignment */
#define KV_GC_RESERVED 1                    /* The reserved block for garbage collection */
#define KV_GC_STACK_SIZE 1024

#define KV_SELF_REMOVE 0
#define KV_ORIG_REMOVE 1
/* Flash block header description */
typedef struct _block_header_t
{
    uint8_t magic; /* The magic number of block */
    uint8_t state; /* The state of the block */
    uint8_t reserved[2];
} __attribute__((packed)) block_hdr_t;

/* Key-value item header description */
typedef struct _item_header_t
{
    uint8_t magic;       /* The magic number of key-value item */
    uint8_t state;       /* The state of key-value item */
    uint8_t crc;         /* The crc-8 value of key-value item */
    uint8_t key_len;     /* The length of the key */
    uint16_t val_len;    /* The length of the value */
    uint16_t origin_off; /* The origin key-value item offset, it will be used when updating */
} __attribute__((packed)) item_hdr_t;

/* Key-value item description */
typedef struct _kv_item_t
{
    item_hdr_t hdr; /* The header of the key-value item, detail see the item_hdr_t structure */
    char *store;    /* The store buffer for key-value */
    uint16_t len;   /* The length of the buffer */
    uint16_t pos;   /* The store position of the key-value item */
} kv_item_t;

/* Block information structure for management */
typedef struct _block_info_t
{
    uint16_t space; /* Free space in current block */
    uint8_t state;  /* The state of current block */
} block_info_t;

typedef struct _kv_mgr_t
{
    uint8_t kv_initialize;  /* The flag to indicate the key-value store is initialized */
    uint8_t gc_triggered;   /* The flag to indicate garbage collection is triggered */
    uint8_t gc_waiter;      /* The number of thread wait for garbage collection finished */
    uint8_t clean_blk_nums; /* The number of block which state is clean */
    uint16_t write_pos;     /* Current write position for key-value item */
    //aos_sem_t gc_sem;
    //aos_mutex_t kv_mutex;
    block_info_t block_info[BLK_NUMS]; /* The array to record block management information */
} kv_mgr_t;

static kv_mgr_t g_kv_mgr;

static const uint8_t BLK_MAGIC_NUM = 'K';  /* The block header magic number */
static const uint8_t ITEM_MAGIC_NUM = 'I'; /* The key-value item header magic number */

void aos_kv_gc(void *arg);


void printf_hex(uint8_t *buf, uint16_t len)
{
    uint16_t i = 0;
    co_printf("----------------------------------------\r\n");
    while(i < len)
    {
        co_printf("  %02X", buf[i]);
        i++;
        if((i%8) == 0)
        {
            co_printf("\r\n");
        }
    }
    co_printf("\r\n----------------------------------------");
}

/* CRC-8: the poly is 0x31 (x^8 + x^5 + x^4 + 1) */
static uint8_t utils_crc8(uint8_t *buf, uint16_t length)
{
    uint8_t crc = 0x00;
    uint8_t i;

    while (length--)
    {
        crc ^= *buf++;
        for (i = 8; i > 0; i--)
        {
            if (crc & 0x80)
            {
                crc = (crc << 1) ^ 0x31;
            }
            else
            {
                crc <<= 1;
            }
        }
    }

    return crc;
}

static int raw_read(uint32_t offset, void *buf, size_t nbytes)
{
    flash_read(KV_PTN+offset, nbytes, buf);
    return RES_OK;
}

static int raw_write(uint32_t offset, const void *buf, size_t nbytes)
{
    flash_write(KV_PTN+offset, nbytes, (uint8_t * )buf);
    return RES_OK;
}

static int raw_erase(uint32_t offset, uint32_t size)
{
    flash_erase(KV_PTN+offset, size);
    return RES_OK;
}

static void trigger_gc(void)
{
    if (g_kv_mgr.gc_triggered)
    {
        return;
    }

    g_kv_mgr.gc_waiter = 0;
    g_kv_mgr.gc_triggered = 1;
    //aos_task_new("kv-gc", aos_kv_gc, NULL, KV_GC_STACK_SIZE);
}

static void kv_item_free(kv_item_t *item)
{
    if (item)
    {
        if (item->store)
        {
            aos_free(item->store);
        }
        aos_free(item);
    }
}

static int kv_state_set(uint16_t pos, uint8_t state)
{
    return raw_write(pos + KV_STATE_OFF, &state, 1);
}


static int kv_block_format(uint8_t index)
{
    block_hdr_t hdr;
    uint16_t pos = index << BLK_BITS;
co_printf("kv_block_format:%d\r\n", index);
    //memset(&hdr, 0, sizeof(hdr));
    memset(&hdr, 0xFF, sizeof(hdr));
    hdr.magic = BLK_MAGIC_NUM;
    if (!raw_erase(pos, BLK_SIZE))
    {
        hdr.state = BLK_STATE_CLEAN;
    }
    else
    {
        return RES_FLASH_EARSE_ERR;
    }

    if (raw_write(pos, &hdr, BLK_HEADER_SIZE) != RES_OK)
    {
        return RES_FLASH_WRITE_ERR;
    }

    g_kv_mgr.block_info[index].state = BLK_STATE_CLEAN;
    g_kv_mgr.block_info[index].space = BLK_SIZE - BLK_HEADER_SIZE;
    (g_kv_mgr.clean_blk_nums)++;
    return RES_OK;
}

static uint16_t kv_item_calc_pos(uint16_t len)
{
    block_info_t *blk_info;
    uint8_t blk_index = (g_kv_mgr.write_pos) >> BLK_BITS;
#if BLK_NUMS > KV_GC_RESERVED + 1
    uint8_t i;
#endif

    blk_info = &(g_kv_mgr.block_info[blk_index]);
    if (blk_info->space > len)
    {
        if (((blk_info->space - len) < ITEM_MAX_LEN) && (g_kv_mgr.clean_blk_nums <= KV_GC_RESERVED))
        {
            trigger_gc();
        }
        return g_kv_mgr.write_pos;
    }

#if BLK_NUMS > KV_GC_RESERVED + 1
    for (i = blk_index + 1; i != blk_index; i++)
    {
        if (i == BLK_NUMS)
        {
            i = 0;
        }

        blk_info = &(g_kv_mgr.block_info[i]);
        if ((blk_info->space) > len)
        {
            g_kv_mgr.write_pos = (i << BLK_BITS) + BLK_SIZE - blk_info->space;
            if (blk_info->state == BLK_STATE_CLEAN)
            {
                if (kv_state_set((i << BLK_BITS), BLK_STATE_USED) != RES_OK)
                {
                    return 0;
                }
                blk_info->state = BLK_STATE_USED;
                (g_kv_mgr.clean_blk_nums)--;
            }
            return g_kv_mgr.write_pos;
        }
    }
#endif

    trigger_gc();
    return 0;
}

static int kv_item_del(kv_item_t *item, int mode)
{
    int ret = RES_OK;
    item_hdr_t hdr;
    char *origin_key = NULL;
    char *new_key = NULL;
    uint8_t i;
    uint16_t offset;

    if (mode == KV_SELF_REMOVE)
    {
        offset = item->pos;
    }
    else if (mode == KV_ORIG_REMOVE)
    {
        offset = item->hdr.origin_off;
        memset(&hdr, 0, ITEM_HEADER_SIZE);
        if (raw_read(offset, &hdr, ITEM_HEADER_SIZE) != RES_OK)
        {
            return RES_FLASH_READ_ERR;
        }

        if ((hdr.magic != ITEM_MAGIC_NUM) ||
            (hdr.state != ITEM_STATE_NORMAL) ||
            (hdr.key_len != item->hdr.key_len))
        {
            return RES_OK;
        }

        origin_key = (char *)aos_malloc(hdr.key_len);
        if (!origin_key)
        {
            return RES_MALLOC_FAILED;
        }
        new_key = (char *)aos_malloc(hdr.key_len);
        if (!new_key)
        {
            aos_free(origin_key);
            return RES_MALLOC_FAILED;
        }

        raw_read(offset + ITEM_HEADER_SIZE, origin_key, hdr.key_len);
        raw_read(item->pos + ITEM_HEADER_SIZE, new_key, hdr.key_len);
        if (memcmp(origin_key, new_key, hdr.key_len) != 0)
        {
            aos_free(origin_key);
            aos_free(new_key);
            return RES_OK;
        }

        aos_free(origin_key);
        aos_free(new_key);
    }
    else
    {
        return RES_INVALID_PARAM;
    }

    if ((ret = kv_state_set(offset, ITEM_STATE_DELETE)) != RES_OK)
    {
        return ret;
    }

    i = offset >> BLK_BITS;
    if (g_kv_mgr.block_info[i].state == BLK_STATE_USED)
    {
        if ((ret = kv_state_set((offset & BLK_OFF_MASK), BLK_STATE_DIRTY)) != RES_OK)
        {
            return ret;
        }
        g_kv_mgr.block_info[i].state = BLK_STATE_DIRTY;
    }

    return ret;
}

/*the function to be invoked while polling the used block*/
typedef int (*item_func)(kv_item_t *item, const char *key);

static int __item_recovery_cb(kv_item_t *item, const char *key)
{
    char *p = (char *)aos_malloc(item->len);
    if (!p)
    {
        return RES_MALLOC_FAILED;
    }

    if (raw_read(item->pos + ITEM_HEADER_SIZE, p, item->len) != RES_OK)
    {
        aos_free(p);
        return RES_FLASH_READ_ERR;
    }

    if (item->hdr.crc == utils_crc8((uint8_t *)p, item->len))
    {
        if ((item->hdr.origin_off != 0) && (item->pos != item->hdr.origin_off))
        {
            kv_item_del(item, KV_ORIG_REMOVE);
        }
    }
    else
    {
        kv_item_del(item, KV_SELF_REMOVE);
    }

    aos_free(p);
    return RES_CONT;
}

static int __item_find_cb(kv_item_t *item, const char *key)
{
    if (item->hdr.key_len != strlen(key))
    {
        return RES_CONT;
    }

    item->store = (char *)aos_malloc(item->hdr.key_len + item->hdr.val_len);
    if (!item->store)
    {
        return RES_MALLOC_FAILED;
    }

    if (raw_read(item->pos + ITEM_HEADER_SIZE, item->store, item->len) != RES_OK)
    {
        return RES_FLASH_READ_ERR;
    }

    if (memcmp(item->store, key, strlen(key)) == 0)
    {
        return RES_OK;
    }

    return RES_CONT;
}

static int __item_gc_cb(kv_item_t *item, const char *key)
{
    char *p;
    int ret;
    uint16_t len;
    uint8_t index;

    len = (ITEM_HEADER_SIZE + item->len + ~KV_ALIGN_MASK) & KV_ALIGN_MASK;
    p = (char *)aos_malloc(len);
    if (!p)
    {
        return RES_MALLOC_FAILED;
    }

    if (raw_read(item->pos, p, len) != RES_OK)
    {
        ret = RES_FLASH_READ_ERR;
        goto err;
    }

    if (raw_write(g_kv_mgr.write_pos, p, len) != RES_OK)
    {
        ret = RES_FLASH_WRITE_ERR;
        goto err;
    }

    g_kv_mgr.write_pos += len;
    index = (g_kv_mgr.write_pos) >> BLK_BITS;
    g_kv_mgr.block_info[index].space -= len;
    ret = RES_CONT;

err:
    aos_free(p);
    return ret;
}

static int __item_del_by_prefix_cb(kv_item_t *item, const char *prefix)
{
    char *key = NULL;
    if (item->hdr.key_len < strlen(prefix))
        return RES_CONT;

    key = (char *)aos_malloc(item->hdr.key_len + 1);
    if (!key)
        return RES_MALLOC_FAILED;

    memset(key, 0, item->hdr.key_len + 1);
    raw_read(item->pos + ITEM_HEADER_SIZE, key, item->hdr.key_len);

    if (strncmp(key, prefix, strlen(prefix)) == 0)
    {
        kv_item_del(item, KV_SELF_REMOVE);
    }

    aos_free(key);
    return RES_CONT;
}

static int __item_del_all_cb(kv_item_t *item, const char *key)
{
    if (item->hdr.key_len > 0)
    {
        kv_item_del(item, KV_SELF_REMOVE);
    }

    return RES_CONT;
}

static kv_item_t *kv_item_traverse(item_func func, uint8_t blk_index, const char *key)
{
    kv_item_t *item;
    item_hdr_t *hdr;
    uint16_t pos = (blk_index << BLK_BITS) + BLK_HEADER_SIZE;
    uint16_t end = (blk_index << BLK_BITS) + BLK_SIZE;
    uint16_t len = 0;
    int ret;

    do
    {
        item = (kv_item_t *)aos_malloc(sizeof(kv_item_t));
        if (!item)
        {
            return NULL;
        }
        memset(item, 0, sizeof(kv_item_t));
        hdr = &(item->hdr);

        if (raw_read(pos, hdr, ITEM_HEADER_SIZE) != RES_OK)
        {
            kv_item_free(item);
            return NULL;
        }

        if (hdr->magic != ITEM_MAGIC_NUM)
        {
            if ((hdr->magic == 0xFF) && (hdr->state == 0xFF))
            {
                kv_item_free(item);
                break;
            }
            hdr->val_len = 0xFFFF;
        }

        if (hdr->val_len > ITEM_MAX_VAL_LEN || hdr->key_len > ITEM_MAX_KEY_LEN ||
            hdr->val_len == 0 || hdr->key_len == 0)
        {
            pos += ITEM_HEADER_SIZE;
            kv_item_free(item);
            if (g_kv_mgr.block_info[blk_index].state == BLK_STATE_USED)
            {
                kv_state_set((blk_index << BLK_BITS), BLK_STATE_DIRTY);
                g_kv_mgr.block_info[blk_index].state = BLK_STATE_DIRTY;
            }
            continue;
        }

        len = (ITEM_HEADER_SIZE + hdr->key_len + hdr->val_len + ~KV_ALIGN_MASK) & KV_ALIGN_MASK;

        if (hdr->state == ITEM_STATE_NORMAL)
        {
            item->pos = pos;
            item->len = hdr->key_len + hdr->val_len;
            ret = func(item, key);
            if (ret == RES_OK)
            {
                return item;
            }
            else if (ret != RES_CONT)
            {
                kv_item_free(item);
                return NULL;
            }
        }
        else
        {
            if (g_kv_mgr.block_info[blk_index].state == BLK_STATE_USED)
            {
                kv_state_set((blk_index << BLK_BITS), BLK_STATE_DIRTY);
                g_kv_mgr.block_info[blk_index].state = BLK_STATE_DIRTY;
            }
        }

        kv_item_free(item);
        pos += len;
    } while (end > (pos + ITEM_HEADER_SIZE));

    g_kv_mgr.block_info[blk_index].space = (end > pos) ? (end - pos) : ITEM_HEADER_SIZE;
    return NULL;
}

static kv_item_t *kv_item_get(const char *key)
{
    kv_item_t *item;
    uint8_t i;

    for (i = 0; i < BLK_NUMS; i++)
    {
        if (g_kv_mgr.block_info[i].state != BLK_STATE_CLEAN)
        {
            item = kv_item_traverse(__item_find_cb, i, key);
            if (item)
            {
                return item;
            }
        }
    }

    return NULL;
}

typedef struct
{
    char *p;
    int ret;
    uint16_t len;
} kv_storeage_t;
static int kv_item_store(const char *key, const void *val, int len, uint16_t origin_off)
{
    kv_storeage_t store;
    item_hdr_t hdr;
    char *p;
    uint16_t pos;
    uint8_t index;
co_printf("%s %d :cur_pos=%08x,origin_off=%08x\r\n", __FUNCTION__, __LINE__,g_kv_mgr.write_pos,origin_off);
    hdr.magic = ITEM_MAGIC_NUM;
    hdr.state = ITEM_STATE_NORMAL;
    hdr.key_len = strlen(key);
    hdr.val_len = len;
    hdr.origin_off = origin_off;

    store.len = (ITEM_HEADER_SIZE + hdr.key_len + hdr.val_len + ~KV_ALIGN_MASK) & KV_ALIGN_MASK;
    store.p = (char *)aos_malloc(store.len);
    if (!store.p)
    {
        return RES_MALLOC_FAILED;
    }

    memset(store.p, 0, store.len);
    p = store.p + ITEM_HEADER_SIZE;
    memcpy(p, key, hdr.key_len);
    p += hdr.key_len;
    memcpy(p, val, hdr.val_len);
    p -= hdr.key_len;
    hdr.crc = utils_crc8((uint8_t *)p, hdr.key_len + hdr.val_len);
    memcpy(store.p, &hdr, ITEM_HEADER_SIZE);

    pos = kv_item_calc_pos(store.len);
co_printf("%s %d len=%d,kv_item_calc_pos=%08x\r\n", __FUNCTION__, __LINE__,store.len,pos);
    if (pos > 0)
    {
        store.ret = raw_write(pos, store.p, store.len);
        if (store.ret == RES_OK)
        {
            g_kv_mgr.write_pos = pos + store.len;
            index = g_kv_mgr.write_pos >> BLK_BITS;
            g_kv_mgr.block_info[index].space -= store.len;
        }
    }
    else
    {
        store.ret = RES_NO_SPACE;
    }

    if (store.p)
    {
        aos_free(store.p);
    }
    return store.ret;
}

static int kv_item_update(kv_item_t *item, const char *key, const void *val, int len)
{
    int ret;

    if (item->hdr.val_len == len)
    {
        if (!memcmp(item->store + item->hdr.key_len, val, len))
        {
            return RES_OK;
        }
    }

    ret = kv_item_store(key, val, len, item->pos);
    if (ret != RES_OK)
    {
        return ret;
    }

    ret = kv_item_del(item, KV_SELF_REMOVE);

    return ret;
}

static int kv_init(void)
{
    block_hdr_t hdr;
    int ret, nums = 0;
    uint8_t i, next;
    uint8_t unclean[BLK_NUMS] = {0};

    for (i = 0; i < BLK_NUMS; i++)
    {
        memset(&hdr, 0, sizeof(block_hdr_t));
        raw_read((i << BLK_BITS), &hdr, BLK_HEADER_SIZE);
co_printf("hdr:%d==>\r\n", i);
printf_hex((uint8_t*)&hdr, BLK_HEADER_SIZE);
        if (hdr.magic == BLK_MAGIC_NUM)
        {
            if (INVALID_BLK_STATE(hdr.state))
            {
                if ((ret = kv_block_format(i)) != RES_OK)
                {
                    return ret;
                }
                else
                {
                    continue;
                }
            }

            g_kv_mgr.block_info[i].state = hdr.state;
            kv_item_traverse(__item_recovery_cb, i, NULL);
            if (hdr.state == BLK_STATE_CLEAN)
            {
                if (g_kv_mgr.block_info[i].space != (BLK_SIZE - BLK_HEADER_SIZE))
                {
                    unclean[nums] = i;
                    nums++;
                }
                else
                {
                    (g_kv_mgr.clean_blk_nums)++;
                }
            }
        }
        else
        {
            if ((ret = kv_block_format(i)) != RES_OK)
            {
                return ret;
            }
        }
    }

    while (nums > 0)
    {
        i = unclean[nums - 1];
        if (g_kv_mgr.clean_blk_nums >= KV_GC_RESERVED)
        {
            if ((ret = kv_state_set((i << BLK_BITS), BLK_STATE_DIRTY)) != RES_OK)
            {
                return ret;
            }
            g_kv_mgr.block_info[i].state = BLK_STATE_DIRTY;
        }
        else
        {
            if ((ret = kv_block_format(i)) != RES_OK)
            {
                return ret;
            }
        }
        nums--;
    }

    if (g_kv_mgr.clean_blk_nums == 0)
    {
        if ((ret = kv_block_format(0)) != RES_OK)
        {
            return ret;
        }
    }

    if (g_kv_mgr.clean_blk_nums == BLK_NUMS)
    {
        g_kv_mgr.write_pos = BLK_HEADER_SIZE;
        if (!kv_state_set((g_kv_mgr.write_pos & BLK_OFF_MASK), BLK_STATE_USED))
        {
            g_kv_mgr.block_info[0].state = BLK_STATE_USED;
            (g_kv_mgr.clean_blk_nums)--;
        }
    }
    else
    {
        for (i = 0; i < BLK_NUMS; i++)
        {
            if ((g_kv_mgr.block_info[i].state == BLK_STATE_USED) ||
                (g_kv_mgr.block_info[i].state == BLK_STATE_DIRTY))
            {
                next = ((i + 1) == BLK_NUMS) ? 0 : (i + 1);
                if (g_kv_mgr.block_info[next].state == BLK_STATE_CLEAN)
                {
                    g_kv_mgr.write_pos = (i << BLK_BITS) + BLK_SIZE - g_kv_mgr.block_info[i].space;
                    break;
                }
            }
        }
    }

    return RES_OK;
}

static void aos_kv_gc(void *arg)
{
    uint8_t i;
    uint8_t gc_index;
    uint8_t gc_copy = 0;
    uint16_t origin_pos;

    /* if (aos_mutex_lock(&(g_kv_mgr.kv_mutex), AOS_WAIT_FOREVER) != 0)
    {
        goto exit;
    } */
co_printf("%s %d\r\n", __FUNCTION__, __LINE__);

    origin_pos = g_kv_mgr.write_pos;
    if (g_kv_mgr.clean_blk_nums == 0)
    {
        goto exit;
    }

    for (gc_index = 0; gc_index < BLK_NUMS; gc_index++)
    {
        if (g_kv_mgr.block_info[gc_index].state == BLK_STATE_CLEAN)
        {
            g_kv_mgr.write_pos = (gc_index << BLK_BITS) + BLK_HEADER_SIZE;
            break;
        }
    }

    if (gc_index == BLK_NUMS)
    {
        goto exit;
    }

    i = (origin_pos >> BLK_BITS) + 1;
    while (1)
    {
        if (i == BLK_NUMS)
        {
            i = 0;
        }

        if (g_kv_mgr.block_info[i].state == BLK_STATE_DIRTY)
        {
co_printf("%s %d:%d==>%d,write_pos=%08x\r\n", __FUNCTION__, __LINE__,i,gc_index,g_kv_mgr.write_pos);
            kv_item_traverse(__item_gc_cb, i, NULL);

            gc_copy = 1;
            if (kv_block_format(i) != RES_OK)
            {
                goto exit;
            }

            kv_state_set((g_kv_mgr.write_pos & BLK_OFF_MASK), BLK_STATE_USED);
            g_kv_mgr.block_info[gc_index].state = BLK_STATE_USED;
            (g_kv_mgr.clean_blk_nums)--;
            break;
        }
        if (i == (origin_pos >> BLK_BITS))
        {
            break;
        }
        i++;
    }

    if (gc_copy == 0)
    {
        g_kv_mgr.write_pos = origin_pos;
    }

exit:
    g_kv_mgr.gc_triggered = 0;
    /* aos_mutex_unlock(&(g_kv_mgr.kv_mutex));
    if (g_kv_mgr.gc_waiter > 0)
    {
        aos_sem_signal_all(&(g_kv_mgr.gc_sem));
    } */

    //aos_task_exit(0);
}

int aos_kv_del(const char *key)
{
    kv_item_t *item;
    int ret;
  /*   if ((ret = aos_mutex_lock(&(g_kv_mgr.kv_mutex), AOS_WAIT_FOREVER)) != RES_OK)
    {
        return ret;
    } */

    item = kv_item_get(key);
    if (!item)
    {
        // aos_mutex_unlock(&(g_kv_mgr.kv_mutex));
        return RES_ITEM_NOT_FOUND;
    }

    ret = kv_item_del(item, KV_SELF_REMOVE);
    kv_item_free(item);
    //aos_mutex_unlock(&(g_kv_mgr.kv_mutex));
    return ret;
}

int aos_kv_del_by_prefix(const char *prefix)
{
    int i;
    /* if ((ret = aos_mutex_lock(&(g_kv_mgr.kv_mutex), AOS_WAIT_FOREVER)) != RES_OK)
    {
        return ret;
    } */

    for (i = 0; i < BLK_NUMS; i++)
    {
        kv_item_traverse(__item_del_by_prefix_cb, i, prefix);
    }

    //aos_mutex_unlock(&(g_kv_mgr.kv_mutex));
    return RES_OK;
}

int aos_kv_del_all(void)
{
    int i, ret;
    /* if ((ret = aos_mutex_lock(&(g_kv_mgr.kv_mutex), AOS_WAIT_FOREVER)) != RES_OK)
    {
        return ret;
    } */

    for (i = 0; i < BLK_NUMS; i++)
    {
        kv_item_traverse(__item_del_all_cb, i, NULL);
    }

    //aos_mutex_unlock(&(g_kv_mgr.kv_mutex));
    return RES_OK;
}

int aos_kv_set(const char *key, const void *val, int len, int sync)
{
    kv_item_t *item;
    int ret;
    if (!key || !val || len <= 0 || strlen(key) > ITEM_MAX_KEY_LEN || len > ITEM_MAX_VAL_LEN)
    {
        return RES_INVALID_PARAM;
    }

   /*  if (g_kv_mgr.gc_triggered)
    {
        (g_kv_mgr.gc_waiter)++;
        aos_sem_wait(&(g_kv_mgr.gc_sem), AOS_WAIT_FOREVER);
    }

    if ((ret = aos_mutex_lock(&(g_kv_mgr.kv_mutex), AOS_WAIT_FOREVER)) != RES_OK)
    {
        return ret;
    } */

    item = kv_item_get(key);
    if (item)
    {
        ret = kv_item_update(item, key, val, len);
        kv_item_free(item);
    }
    else
    {
        ret = kv_item_store(key, val, len, 0);
    }

    if(g_kv_mgr.gc_triggered)                            //200825 NiuJG修改为同步执行
    {
        aos_kv_gc(0);         
    }
    
    //aos_mutex_unlock(&(g_kv_mgr.kv_mutex));
    return ret;
}

int aos_kv_get(const char *key, void *buffer, int *buffer_len)
{
    kv_item_t *item = NULL;
    int ret;

    if (!key || !buffer || !buffer_len || *buffer_len <= 0)
    {
        return RES_INVALID_PARAM;
    }

    /* if ((ret = aos_mutex_lock(&(g_kv_mgr.kv_mutex), AOS_WAIT_FOREVER)) != RES_OK)
    {
        return ret;
    } */

    item = kv_item_get(key);

    //aos_mutex_unlock(&(g_kv_mgr.kv_mutex));

    if (!item)
    {
        return RES_ITEM_NOT_FOUND;
    }
co_printf("aos_kv_get pos=%08x,key=%s,len=%d\r\n", item->pos,key,item->len);

    if (*buffer_len < item->hdr.val_len)
    {
        *buffer_len = item->hdr.val_len;
        kv_item_free(item);
        return RES_NO_SPACE;
    }
    else
    {
        memcpy(buffer, (item->store + item->hdr.key_len), item->hdr.val_len);
        *buffer_len = item->hdr.val_len;
    }

    kv_item_free(item);
    return RES_OK;
}




/*
 * 200826 NiuJG
 * 自定义默认设置的存储和载入
 */
static int kv_block_replica(uint16_t offset_src, uint16_t offset_dst)
{
    kv_item_t *item;
    item_hdr_t *hdr;
    uint16_t pos = offset_src + BLK_HEADER_SIZE;
    uint16_t end = offset_src + BLK_SIZE;
    uint16_t len = 0;
    int cnt = 0;

    block_hdr_t block_hdr;

    /* 检查block有效性 */
    memset(&block_hdr, 0xFF, sizeof(block_hdr_t));
    raw_read(offset_src, &block_hdr, BLK_HEADER_SIZE);

    if (block_hdr.magic!=BLK_MAGIC_NUM || INVALID_BLK_STATE(block_hdr.state))
    {
        return -1;
    }

    /* 拷贝写块头 */
    block_hdr.state = BLK_STATE_USED;
    block_hdr.reserved[0] = 0xFF;
    if (raw_erase(offset_dst, BLK_SIZE) != RES_OK)
    {
        return -3;
    }
    if (raw_write(offset_dst, &block_hdr, BLK_HEADER_SIZE) != RES_OK)
    {
        return -3;
    }

    /* 拷贝有效item到目的block中 */
    do
    {
        item = (kv_item_t *)aos_malloc(sizeof(kv_item_t));
        if (!item)
        {
            return -2;
        }
        memset(item, 0, sizeof(kv_item_t));
        hdr = &(item->hdr);

        if (raw_read(pos, hdr, ITEM_HEADER_SIZE) != RES_OK)
        {
            kv_item_free(item);
            return -3;
        }

        if (hdr->magic != ITEM_MAGIC_NUM)
        {
            if ((hdr->magic == 0xFF) && (hdr->state == 0xFF))
            {
                kv_item_free(item);
                break;
            }
            hdr->val_len = 0xFFFF;
        }

        if (hdr->val_len > ITEM_MAX_VAL_LEN || hdr->key_len > ITEM_MAX_KEY_LEN ||
            hdr->val_len == 0 || hdr->key_len == 0)
        {
            pos += ITEM_HEADER_SIZE;
            kv_item_free(item);
            continue;
        }

        len = (ITEM_HEADER_SIZE + hdr->key_len + hdr->val_len + ~KV_ALIGN_MASK) & KV_ALIGN_MASK;

        if (hdr->state == ITEM_STATE_NORMAL)
        {
            item->pos = pos;
            item->len = hdr->key_len + hdr->val_len;

            item->store = (char *)aos_malloc(len);
            if (!item->store)
            {
                kv_item_free(item);
                return -2;
            }
            if (raw_read(item->pos, item->store, len) != RES_OK)
            {
                kv_item_free(item);
                return -3;
            }
            if (raw_write(offset_dst, item->store, len) != RES_OK)
            {
                kv_item_free(item);
                return -3;
            }

            cnt++;
            offset_dst += len;
        }

        kv_item_free(item);
        pos += len;
    } while (end > (pos + ITEM_HEADER_SIZE));
    
    return cnt;
}


int kv_default_custom_load()
{
co_printf("==>kv_default_custom_load\r\n");   
    kv_state_set((1 << BLK_BITS) , 0x00);        //使失效
    int ret = kv_block_replica(KV_DEFAULT_CUSTOM_PTN-KV_PTN, 0);   
    if(ret < 0)
    {
        kv_default_factory_load();
        kv_default_custom_store();
        return 1;
    }

    aos_kv_deinit();
    aos_kv_init();
    return 0;
}

int kv_default_custom_store()
{
co_printf("==>int kv_default_custom_store\r\n"); 
    return kv_block_replica(0, KV_DEFAULT_CUSTOM_PTN-KV_PTN);   
}


int kv_default_factory_load()
{
co_printf("==>int kv_default_factory_load\r\n");     
    kv_state_set((0 << BLK_BITS) , 0x00);
    kv_state_set((1 << BLK_BITS) , 0x00);

    aos_kv_deinit();
    aos_kv_init();
}


int kv_check_has_val()
{
    return (g_kv_mgr.write_pos==BLK_HEADER_SIZE) ? 0 : 1;
}


/* CLI Support */
#ifdef CONFIG_AOS_CLI

#ifdef SUPPORT_KV_LIST_CMD
static int __item_print_cb(kv_item_t *item, const char *key)
{
    char *p_key = NULL;
    char *p_val = NULL;
    p_key = (char *)aos_malloc(item->hdr.key_len + 1);
    if (!p_key)
    {
        return RES_MALLOC_FAILED;
    }
    memset(p_key, 0, item->hdr.key_len + 1);
    raw_read(item->pos + ITEM_HEADER_SIZE, p_key, item->hdr.key_len);

    p_val = (char *)aos_malloc(item->hdr.val_len + 1);
    if (!p_val)
    {
        aos_free(p_key);
        return RES_MALLOC_FAILED;
    }
    memset(p_val, 0, item->hdr.val_len + 1);
    raw_read(item->pos + ITEM_HEADER_SIZE + item->hdr.key_len, p_val, item->hdr.val_len);

    aos_cli_printf("[%04d]%s=%s\r\n", item->hdr.val_len, p_key, p_val);

    aos_free(p_key);
    aos_free(p_val);

    return RES_CONT;
}
#endif

static int _kv_getx_cmd(const char *key, kv_get_type_e get_type)
{
    int ret = 0;
    int index = 0;
    int len = ITEM_MAX_LEN;
    unsigned char *buffer = NULL;
    kv_item_t *item = NULL;

    /* if ((ret = aos_mutex_lock(&(g_kv_mgr.kv_mutex), AOS_WAIT_FOREVER)) != RES_OK)
    {
        return -1;
    } */

    item = kv_item_get(key);

    //aos_mutex_unlock(&(g_kv_mgr.kv_mutex));

    if (!item)
    {
        aos_cli_printf("key:%s not found\r\n", key);
        return -1;
    }

    len = item->hdr.val_len + 1;

    buffer = aos_malloc(len + 1);
    kv_item_free(item);

    if (!buffer)
    {
        aos_cli_printf("no mem\r\n");
        return -1;
    }

    memset(buffer, 0, len);

    ret = aos_kv_get(key, buffer, &len);
    if (ret != 0)
    {
        aos_cli_printf("cli: no paired kv\r\n");
    }
    else
    {
        switch (get_type)
        {
        case KV_GET_TYPE_STRING:
        {
            aos_cli_printf("value is %s\r\n", buffer);
        }
        break;
        case KV_GET_TYPE_BINARY:
        {
            aos_cli_printf("\r\nkv key:%s binary value is\r\n", key);

            for (index = 0; index < len; index++)
            {
                aos_cli_printf("0x%02x ", buffer[index]);
                if ((index + 1) % 16 == 0)
                {
                    aos_cli_printf("\r\n");
                }
            }
        }
        break;
        default:
            break;
        }
    }

    if (buffer)
    {
        aos_free(buffer);
    }

    return 0;
}

static void handle_kv_cmd(char *pwbuf, int blen, int argc, char **argv)
{
    const char *rtype = argc > 1 ? argv[1] : "";
    int ret = 0;

#ifdef SUPPORT_KV_LIST_CMD
    int i = 0;
#endif

    if (strcmp(rtype, "set") == 0)
    {
        if (argc != 4)
        {
            return;
        }
        ret = aos_kv_set(argv[2], argv[3], strlen(argv[3]), 1);
        if (ret != 0)
        {
            aos_cli_printf("cli set kv failed\r\n");
        }
    }
    else if (strcmp(rtype, "get") == 0)
    {
        if (argc != 3)
        {
            return;
        }

        _kv_getx_cmd(argv[2], KV_GET_TYPE_STRING);
    }
    else if (strcmp(rtype, "getb") == 0)
    {
        if (argc != 3)
        {
            return;
        }

        _kv_getx_cmd(argv[2], KV_GET_TYPE_BINARY);
    }
    else if (strcmp(rtype, "del") == 0)
    {
        if (argc != 3)
        {
            return;
        }
        ret = aos_kv_del(argv[2]);
        if (ret != 0)
        {
            aos_cli_printf("cli kv del failed\r\n");
        }
#ifdef SUPPORT_KV_LIST_CMD
    }
    else if (strcmp(rtype, "list") == 0)
    {
        for (i = 0; i < BLK_NUMS; i++)
        {
            kv_item_traverse(__item_print_cb, i, NULL);
        }
#endif
    }
    else if (strcmp(rtype, "clear") == 0)
    {
        aos_kv_del_all();
    }
    else
    {
        aos_cli_printf("\"kv %s\" not support!\r\n", rtype);
    }

    return;
}

static struct cli_command ncmd = {
    "kv",
#ifdef SUPPORT_KV_LIST_CMD
    "kv [set key value | get key | getb key | del key | list | clear]",
#else
    "kv [set key value | get key | getb key | del key | clear]",
#endif
    handle_kv_cmd};
#endif

int aos_kv_init(void)
{
    uint8_t blk_index;
    int ret;

    if (g_kv_mgr.kv_initialize)
    {
        return RES_OK;
    }

    if (BLK_NUMS <= KV_GC_RESERVED)
    {
        return -EINVAL;
    }

    memset(&g_kv_mgr, 0, sizeof(g_kv_mgr));
    /* if ((ret = aos_mutex_new(&(g_kv_mgr.kv_mutex))) != 0)
    {
        return ret;
    }
 */
#ifdef CONFIG_AOS_CLI
    aos_cli_register_command(&ncmd);
#endif

    if ((ret = kv_init()) != RES_OK)
    {
        return ret;
    }

    /* if ((ret = aos_sem_new(&(g_kv_mgr.gc_sem), 0)) != RES_OK)
    {
        return ret;
    }
 */
    g_kv_mgr.kv_initialize = 1;
  
    blk_index = (g_kv_mgr.write_pos >> BLK_BITS);
    if (((g_kv_mgr.block_info[blk_index].space) < ITEM_MAX_LEN) &&
        (g_kv_mgr.clean_blk_nums < KV_GC_RESERVED + 1))
    {
        trigger_gc();
    }

    return RES_OK;
}

void aos_kv_deinit(void)
{
    g_kv_mgr.kv_initialize = 0;
    //aos_sem_free(&(g_kv_mgr.gc_sem));
    //aos_mutex_free(&(g_kv_mgr.kv_mutex));
}
