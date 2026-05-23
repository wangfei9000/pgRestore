/*
 * wf.c -- 从 PostgreSQL 数据目录直接读取数据的独立工具
 *
 * 功能: 传入 PGDATA 路径，解析数据目录结构，打印数据库数量和基本信息。
 *
 * 编译: cc -o wf wf.c
 * gcc -Wall -Wextra -std=c99 -Wno-unused-function -Wno-unused-const-variable -o wf wf.c
 * 用法: ./wf /var/lib/postgresql/16/main
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <ctype.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>

/* ================================================================
 * 基本类型定义 (摘自 pgsql c.h)
 * ================================================================ */

typedef unsigned char   uint8;
typedef signed char     int8;
typedef unsigned short  uint16;
typedef signed short    int16;
typedef unsigned int    uint32;
typedef signed int      int32;
typedef unsigned long long uint64;
typedef signed long long int64;

typedef uint32  Oid;
typedef uint32  BlockNumber;
typedef uint16  OffsetNumber;
typedef uint32  TransactionId;

#define BLCKSZ          8192
#define MAXIMUM_ALIGNOF 8
#define MAXALIGN(LEN)   (((uintptr_t)(LEN) + 7) & ~((uintptr_t)7))

#define PG_VERSION_FILE "PG_VERSION"
#define OPEN_DEBUG_LOG 1  // 1 关闭，0 打开

/* ================================================================
 * 页面层结构体 (摘自 storage/ 下的头文件)
 * ================================================================ */

typedef struct ItemIdData
{
    unsigned lp_off:15;
    unsigned lp_flags:2;
    unsigned lp_len:15;
} ItemIdData;

#define LP_UNUSED    0
#define LP_NORMAL    1
#define LP_REDIRECT  2
#define LP_DEAD      3

typedef struct ItemPointerData
{
    uint16 ip_blkid_hi;
    uint16 ip_blkid_lo;
    uint16 ip_posid;
} ItemPointerData;

typedef struct PageXLogRecPtr
{
    uint32 xlogid;
    uint32 xrecoff;
} PageXLogRecPtr;

typedef struct PageHeaderData
{
    PageXLogRecPtr  pd_lsn;
    uint16          pd_checksum;
    uint16          pd_flags;
    uint16          pd_lower;
    uint16          pd_upper;
    uint16          pd_special;
    uint16          pd_pagesize_version;
    TransactionId   pd_prune_xid;
    ItemIdData      pd_linp[];
} PageHeaderData;

#define SizeOfPageHeaderData (offsetof(PageHeaderData, pd_linp))

static inline OffsetNumber
PageGetMaxOffsetNumber(char *page)
{
    PageHeaderData *hdr = (PageHeaderData *)page;
    if (hdr->pd_lower <= SizeOfPageHeaderData)
        return 0;
    return (hdr->pd_lower - SizeOfPageHeaderData) / sizeof(ItemIdData);
}

static inline ItemIdData *
PageGetItemId(char *page, OffsetNumber offnum)
{
    return &((PageHeaderData *)page)->pd_linp[offnum - 1];
}

static inline char *
PageGetItem(char *page, ItemIdData *id)
{
    return page + id->lp_off;
}

/* ================================================================
 * Tuple 层结构体 (摘自 access/htup*.h)
 * ================================================================ */

typedef struct HeapTupleFields
{
    TransactionId t_xmin;
    TransactionId t_xmax;
    union {
        uint32 t_cid;
        TransactionId t_xvac;
    } t_field3;
} HeapTupleFields;

typedef struct DatumTupleFields
{
    int32 datum_len_;
    int32 datum_typmod;
    Oid   datum_typeid;
} DatumTupleFields;

typedef struct HeapTupleHeaderData
{
    union {
        HeapTupleFields  t_heap;
        DatumTupleFields t_datum;
    } t_choice;

    ItemPointerData t_ctid;
    uint16  t_infomask2;
    uint16  t_infomask;
    uint8   t_hoff;
    uint8   t_bits[];
} HeapTupleHeaderData;

#define SizeofHeapTupleHeader offsetof(HeapTupleHeaderData, t_bits)

/* t_infomask 标志 */
#define HEAP_HASNULL        0x0001
#define HEAP_HASVARWIDTH    0x0002
#define HEAP_HASEXTERNAL    0x0004
#define HEAP_XMIN_COMMITTED 0x0100
#define HEAP_XMIN_INVALID   0x0200
#define HEAP_XMAX_COMMITTED 0x0400
#define HEAP_XMAX_INVALID   0x0800
#define HEAP_UPDATED        0x2000

#define HEAP_NATTS_MASK     0x07FF

/* 从 tuple 头快速取值 */
#define HeapTupleHeaderGetNatts(tup)  ((tup)->t_infomask2 & HEAP_NATTS_MASK)
#define HeapTupleHeaderHasNulls(tup)  (((tup)->t_infomask & HEAP_HASNULL) != 0)

#define BITMAPLEN(NATTS)  (((int)(NATTS) + 7) / 8)

/* ================================================================
 * 系统表常量
 * ================================================================ */

/* pg_database 系统表: OID=1262, 存储在 global/ 目录 */
#define DATABASE_REL_OID  1262

/*
 * pg_database 列布局 (PG17, 共 18 个用户列):
 *   1  oid              Oid       4   i  byval
 *   2  datname          NameData  64  c  byref
 *   3  datdba           Oid       4   i  byval
 *   4  encoding         int32     4   i  byval
 *   5  datlocprovider   char      1   c  byval
 *   6  datistemplate    bool      1   c  byval
 *   7  datallowconn     bool      1   c  byval
 *   8  dathasloginevt   bool      1   c  byval
 *   9  datconnlimit     int32     4   i  byval
 *  10  datfrozenxid     Xid       4   i  byval
 *  11  datminmxid       Xid       4   i  byval
 *  12  dattablespace    Oid       4   i  byval
 *  13  datcollate       text     -1   i  byref  (varlena)
 *  14  datctype         text     -1   i  byref  (varlena)
 *  15  datlocale        text     -1   i  byref  (varlena)
 *  16  daticurules      text     -1   i  byref  (varlena)
 *  17  datcollversion   text     -1   i  byref  (varlena)
 *  18  datacl           aclitem  -1   i  byref  (varlena)
 */
#define Natts_pg_database             18
#define Anum_pg_database_oid           1
#define Anum_pg_database_datname       2
#define Anum_pg_database_datdba        3
#define Anum_pg_database_encoding      4
#define Anum_pg_database_datistemplate 6
#define Anum_pg_database_datallowconn  7
#define Anum_pg_database_datconnlimit  9
#define Anum_pg_database_dattablespace 12

/* pg_class 系统表: OID=1259 (每库独立) */
#define CLASS_REL_OID     1259
/*
 * pg_class 列布局 (PG17, 30个定长列 + 3个变长列):
 *   1  oid               Oid       4   i  byval
 *   2  relname           NameData  64  c  byref
 *   3  relnamespace      Oid       4   i  byval
 *   4  reltype           Oid       4   i  byval
 *   5  reloftype         Oid       4   i  byval
 *   6  relowner          Oid       4   i  byval
 *   7  relam             Oid       4   i  byval
 *   8  relfilenode       Oid       4   i  byval
 *   9  reltablespace     Oid       4   i  byval
 *  10  relpages          int32     4   i  byval
 *  11  reltuples         float4    4   i  byval
 *  12  relallvisible     int32     4   i  byval
 *  13  reltoastrelid     Oid       4   i  byval
 *  14  relhasindex       bool      1   c  byval
 *  15  relisshared       bool      1   c  byval
 *  16  relpersistence    char      1   c  byval
 *  17  relkind           char      1   c  byval
 *  18  relnatts          int16     2   s  byval
 *  19  relchecks         int16     2   s  byval
 *  20  relhasrules       bool      1   c  byval
 *  21  relhastriggers    bool      1   c  byval
 *  22  relhassubclass    bool      1   c  byval
 *  23  relrowsecurity    bool      1   c  byval
 *  24  relforcerowsecurity bool    1   c  byval
 *  25  relispopulated    bool      1   c  byval
 *  26  relreplident      char      1   c  byval
 *  27  relispartition    bool      1   c  byval
 *  28  relrewrite        Oid       4   i  byval
 *  29  relfrozenxid      Xid       4   i  byval
 *  30  relminmxid        Xid       4   i  byval
 *  -- 变长部分 --
 *  31  relacl            aclitem  -1   i  byref
 *  32  reloptions        text     -1   i  byref
 *  33  relpartbound      pg_node_tree -1 i byref
 */
#define Natts_pg_class              33
#define Anum_pg_class_relname        2
#define Anum_pg_class_relnamespace   3
#define Anum_pg_class_relkind       17
#define Anum_pg_class_relnatts      18
#define Anum_pg_class_relpages      10
#define Anum_pg_class_reltuples     11
#define Anum_pg_class_reltoastrelid 13
#define Anum_pg_class_relhasindex   14
#define Anum_pg_class_relfilenode    8

static const int   cl_attlen[]   = {
    4, 64, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 1, 1, 1, 1, 2, 2,
    1, 1, 1, 1, 1, 1, 1, 1, 4, 4, 4, -1, -1, -1
};
static const char  cl_attalign[] = {
    'i', 'c', 'i', 'i', 'i', 'i', 'i', 'i', 'i', 'i', 'i', 'i', 'i',
    'c', 'c', 'c', 'c', 's', 's',
    'c', 'c', 'c', 'c', 'c', 'c', 'c', 'c', 'i', 'i', 'i',
    'i', 'i', 'i'
};

/* ================================================================
 * 工具函数
 * ================================================================ */

/* 检查 null bitmap 中某列是否为 NULL (bit=0 表示 null) */
static inline int
att_isnull(int attnum, const uint8 *bits)
{
    return !(bits[attnum >> 3] & (1 << (attnum & 0x07)));
}

/* 对齐计算 */
static inline uintptr_t
att_align_nominal(uintptr_t cur_offset, char attalign)
{
    switch (attalign) {
        case 'c': return cur_offset;                    /* char: 不对齐 */
        case 's': return (cur_offset + 1) & ~(uintptr_t)1;  /* short: 2字节 */
        case 'i': return (cur_offset + 3) & ~(uintptr_t)3;  /* int:   4字节 */
        case 'd': return (cur_offset + 7) & ~(uintptr_t)7;  /* double:8字节 */
        default:  return cur_offset;
    }
}

static inline int
att_addlength(int cur_offset, int attlen, char *attptr)
{
    if (attlen > 0)
        return cur_offset + attlen;
    else if (attlen == -1) {
        /* varlena: 检查 1字节头还是4字节头 */
        if ((((uint8 *)attptr)[0] & 0x01) == 0x01)
            return cur_offset + (((uint8 *)attptr)[0] >> 1);       /* 1字节头: va_header = (len<<1)|1 */
        else
            return cur_offset + (*(uint32 *)attptr >> 2);          /* 4字节头: va_header = (len<<2)|2 */
    } else if (attlen == -2)
        return cur_offset + strlen(attptr) + 1;             /* cstring */
    return cur_offset;
}

/* 验证目录是否是合法的 PGDATA */
static int
validate_pgdata(const char *datadir)
{
    char path[1024];
    FILE *fp;

    snprintf(path, sizeof(path), "%s/%s", datadir, PG_VERSION_FILE);
    fp = fopen(path, "r");
    if (!fp) {
        fprintf(stderr, "错误: '%s' 不是有效的 PGDATA 目录 (无 %s)\n",
                datadir, PG_VERSION_FILE);
        return 0;
    }
    fclose(fp);

    snprintf(path, sizeof(path), "%s/base", datadir);
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "错误: '%s' 下没有 base/ 目录\n", datadir);
        return 0;
    }

    snprintf(path, sizeof(path), "%s/global", datadir);
    if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "错误: '%s' 下没有 global/ 目录\n", datadir);
        return 0;
    }

    return 1;
}

/* 判断字符串是否全为数字 */
static int
is_numeric(const char *s)
{
    if (!s || !*s) return 0;
    while (*s) {
        if (!isdigit((unsigned char)*s)) return 0;
        s++;
    }
    return 1;
}

/* 从 base/ 目录统计 database 数量（简单方案） */
static int
count_databases_simple(const char *datadir)
{
    char basepath[1024];
    DIR *dir;
    struct dirent *ent;
    int count = 0;

    snprintf(basepath, sizeof(basepath), "%s/base", datadir);
    dir = opendir(basepath);
    if (!dir) {
        perror("opendir base/");
        return -1;
    }

    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.')
            continue;

        /* 数据库子目录名是数字 OID */
        if (!is_numeric(ent->d_name))
            continue;

        /* 确认是目录而不是文件 */
        char fullpath[2048];
        struct stat st;
        snprintf(fullpath, sizeof(fullpath), "%s/%s", basepath, ent->d_name);
        if (stat(fullpath, &st) == 0 && S_ISDIR(st.st_mode)) {
            count++;
            printf("  [db %3d] OID=%s\n", count, ent->d_name);
        }
    }
    closedir(dir);
    return count;
}

/* 读取一个文件到内存 */
static char *
read_file(const char *path, size_t *size_out)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;

    fseek(fp, 0, SEEK_END);
    size_t size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    char *buf = malloc(size);
    if (!buf) { fclose(fp); return NULL; }

    if (fread(buf, 1, size, fp) != size) {
        free(buf);
        fclose(fp);
        return NULL;
    }
    fclose(fp);
    *size_out = size;
    return buf;
}

/* 读取某个文件的第 N 个页面 */
static char *
read_page(const char *datadir, const char *subdir,
          Oid relfilenode, BlockNumber blkno)
{
    char path[2048];
    char *buf;
    int fd;

    /* 主文件路径 */
    snprintf(path, sizeof(path), "%s/%s/%u", datadir, subdir, relfilenode);

    fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;

    off_t offset = (off_t)blkno * BLCKSZ;
    if (lseek(fd, offset, SEEK_SET) != offset) {
        close(fd);
        return NULL;
    }

    buf = malloc(BLCKSZ);
    if (!buf) { close(fd); return NULL; }

    ssize_t n = read(fd, buf, BLCKSZ);
    close(fd);

    if (n != BLCKSZ) {
        free(buf);
        return NULL;
    }
    return buf;
}

/* 从 pg_database 系统表中解析数据库名 */
typedef struct {
    Oid     oid;
    char    name[64];
    Oid     owner;
    int32   encoding;
    int     istemplate;
    int     allowconn;
    int32   connlimit;
    Oid     tablespace;
    char    collate[128];
    char    ctype[128];
} DBEntry;

#define MAX_DATABASES 1024

/* 每列的 attlen/attalign (下标=attnum-1, 共18列) */
static const int   db_attlen[]   = {4, 64, 4, 4, 1, 1, 1, 1, 4, 4, 4, 4, -1, -1, -1, -1, -1, -1};
static const char  db_attalign[] = {'i', 'c', 'i', 'i', 'c', 'c', 'c', 'c', 'i', 'i', 'i', 'i',
                                    'i', 'i', 'i', 'i', 'i', 'i'};

/*
 * 获取文件的页面数
 */
static BlockNumber
get_file_pages(const char *datadir, const char *subdir, Oid relfilenode)
{
    char path[2048];
    struct stat st;

    snprintf(path, sizeof(path), "%s/%s/%u", datadir, subdir, relfilenode);
    if (stat(path, &st) != 0) return 0;
    if (st.st_size == 0) return 0;
    return (BlockNumber)(st.st_size / BLCKSZ);
}

/*
 * 从 pg_database 系统表 (global/1262) 中解析所有数据库信息。
 *
 * pg_database 是共享系统表，存储在 PGDATA/global/1262。
 * 表中有 18 个用户列 (固定部分12个 + 变长部分6个)。
 * 该表的 relhasoids 在旧版本为 true，oid 列作为系统列存在。
 *
 * 每个 tuple 的 on-disk 结构:
 *   [HeapTupleHeaderData 固定头 23B]
 *   [null bitmap (如果 HEAP_HASNULL)]
 *   [padding 至 MAXALIGN]
 *   [col1 数据][col2 数据]...[colN 数据]  ← 从 t_hoff 处开始
 */
static int
parse_pg_database(const char *datadir, DBEntry *entries, int max_entries)
{
    BlockNumber npages = get_file_pages(datadir, "global", DATABASE_REL_OID);
    if (npages == 0) {
        fprintf(stderr, "无法读取 pg_database 表 (global/%u)\n", DATABASE_REL_OID);
        return 0;
    }

    int count = 0;

    for (BlockNumber blk = 0; blk < npages && count < max_entries; blk++)
    {
        char *page = read_page(datadir, "global", DATABASE_REL_OID, blk);
        if (!page) continue;

        OffsetNumber maxoff = PageGetMaxOffsetNumber(page);

        for (OffsetNumber i = 1; i <= maxoff && count < max_entries; i++)
        {
            ItemIdData *id = PageGetItemId(page, i);
            if (id->lp_flags != LP_NORMAL || id->lp_len == 0)
                continue;

            HeapTupleHeaderData *tup = (HeapTupleHeaderData *)PageGetItem(page, id);

            /* 跳过已删除的 tuple (xmax 有效) */
            if (!(tup->t_infomask & HEAP_XMAX_INVALID) &&
                 (tup->t_infomask & (HEAP_XMAX_COMMITTED | HEAP_XMAX_INVALID)) != HEAP_XMAX_INVALID)
                continue;

            int tup_natts = HeapTupleHeaderGetNatts(tup);
            if (tup_natts < 2) continue;

            int has_nulls = (tup->t_infomask & HEAP_HASNULL) != 0;
            uint8 *null_bits = tup->t_bits;
            char *tp = (char *)tup + tup->t_hoff;
            uintptr_t off = 0;

            DBEntry *e = &entries[count];
            memset(e, 0, sizeof(DBEntry));
            int got_name = 0;

            int ncols = (Natts_pg_database < tup_natts) ? Natts_pg_database : tup_natts;

            for (int attnum = 0; attnum < ncols; attnum++)
            {
                if (has_nulls && att_isnull(attnum, null_bits))
                    continue;

                off = att_align_nominal(off, db_attalign[attnum]);
                char *data = tp + off;

                if (attnum == Anum_pg_database_oid - 1) {
                    e->oid = *(Oid *)data;
                } else if (attnum == Anum_pg_database_datname - 1) {
                    memcpy(e->name, data, 64);
                    e->name[63] = '\0';
                    got_name = 1;
                } else if (attnum == Anum_pg_database_datdba - 1) {
                    e->owner = *(Oid *)data;
                } else if (attnum == Anum_pg_database_encoding - 1) {
                    e->encoding = *(int32 *)data;
                } else if (attnum == Anum_pg_database_datistemplate - 1) {
                    e->istemplate = *(uint8 *)data;
                } else if (attnum == Anum_pg_database_datallowconn - 1) {
                    e->allowconn = *(uint8 *)data;
                } else if (attnum == Anum_pg_database_datconnlimit - 1) {
                    e->connlimit = *(int32 *)data;
                } else if (attnum == Anum_pg_database_dattablespace - 1) {
                    e->tablespace = *(Oid *)data;
                } else if (attnum == 12) {  /* datcollate (text, varlena) */
                    int is_1b = (*(uint8 *)data & 0x01);
                    int total_len = is_1b ? (*(uint8 *)data >> 1) : (*(uint32 *)data >> 2);
                    int data_len = total_len - (is_1b ? 1 : 4);
                    int copylen = (data_len > 127) ? 127 : data_len;
                    if (data_len > 0) {
                        memcpy(e->collate, data + (is_1b ? 1 : 4), copylen);
                        e->collate[copylen] = '\0';
                    }
                } else if (attnum == 13) {  /* datctype (text, varlena) */
                    int is_1b = (*(uint8 *)data & 0x01);
                    int total_len = is_1b ? (*(uint8 *)data >> 1) : (*(uint32 *)data >> 2);
                    int data_len = total_len - (is_1b ? 1 : 4);
                    int copylen = (data_len > 127) ? 127 : data_len;
                    if (data_len > 0) {
                        memcpy(e->ctype, data + (is_1b ? 1 : 4), copylen);
                        e->ctype[copylen] = '\0';
                    }
                }

                off = att_addlength(off, db_attlen[attnum], data);
            }

            if (got_name && e->name[0] != '\0')
                count++;
            else
                memset(e, 0, sizeof(DBEntry));  /* 回退: 无效条目 */
        }
        free(page);
    }

    return count;
}

/*
 * 根据数据库名查找其 OID
 */
static Oid
find_database_oid(const char *datadir, const char *dbname)
{
    BlockNumber npages = get_file_pages(datadir, "global", DATABASE_REL_OID);
    if (npages == 0) return 0;

    for (BlockNumber blk = 0; blk < npages; blk++)
    {
        char *page = read_page(datadir, "global", DATABASE_REL_OID, blk);
        if (!page) continue;

        OffsetNumber maxoff = PageGetMaxOffsetNumber(page);
        for (OffsetNumber i = 1; i <= maxoff; i++)
        {
            ItemIdData *id = PageGetItemId(page, i);
            if (id->lp_flags != LP_NORMAL || id->lp_len == 0) continue;

            HeapTupleHeaderData *tup = (HeapTupleHeaderData *)PageGetItem(page, id);
            if (!(tup->t_infomask & HEAP_XMAX_INVALID) &&
                 (tup->t_infomask & (HEAP_XMAX_COMMITTED | HEAP_XMAX_INVALID)) != HEAP_XMAX_INVALID)
                continue;

            int tup_natts = HeapTupleHeaderGetNatts(tup);
            if (tup_natts < 2) continue;

            int has_nulls = (tup->t_infomask & HEAP_HASNULL) != 0;
            uint8 *null_bits = tup->t_bits;
            char *tp = (char *)tup + tup->t_hoff;
            uintptr_t off = 0;

            char name[64] = {0};
            Oid  found_oid = 0;
            int  ncols = (Natts_pg_database < tup_natts) ? Natts_pg_database : tup_natts;

            for (int attnum = 0; attnum < ncols; attnum++)
            {
                if (has_nulls && att_isnull(attnum, null_bits))
                    continue;
                off = att_align_nominal(off, db_attalign[attnum]);
                char *data = tp + off;

                if (attnum == 0)  /* oid */
                    found_oid = *(Oid *)data;
                else if (attnum == 1) {  /* datname */
                    memcpy(name, data, 64);
                    name[63] = '\0';
                }
                off = att_addlength(off, db_attlen[attnum], data);
            }

            if (found_oid && strcmp(name, dbname) == 0) {
                free(page);
                return found_oid;
            }
        }
        free(page);
    }
    return 0;
}

/*
 * 列出指定数据库中的所有表 (从 pg_class)
 */
static void
list_tables(const char *datadir, Oid dboid, int verbose, int table_only)
{
    char subdir[32];
    snprintf(subdir, sizeof(subdir), "base/%u", dboid);

    BlockNumber npages = get_file_pages(datadir, subdir, CLASS_REL_OID);
    if (npages == 0) {
        printf("数据库 %u 的 pg_class 为空或无法读取\n", dboid);
        return;
    }

    /* 统计 */
    int total = 0, ntables = 0, nindexes = 0, nviews = 0;
    int nseqs = 0, nmatviews = 0, npart = 0, nforeign = 0;

    printf("%-4s %-34s %-6s %-8s %-5s %s\n",
           "序号", "表名", "OID", "relkind", "列数", "页/行");
    printf("%-4s %-34s %-6s %-8s %-5s %s\n",
           "----", "----------------------------------", "------",
           "--------", "-----", "------");

    for (BlockNumber blk = 0; blk < npages; blk++)
    {
        char *page = read_page(datadir, subdir, CLASS_REL_OID, blk);
        if (!page) continue;

        OffsetNumber maxoff = PageGetMaxOffsetNumber(page);
        for (OffsetNumber i = 1; i <= maxoff; i++)
        {
            ItemIdData *id = PageGetItemId(page, i);
            if (id->lp_flags != LP_NORMAL || id->lp_len == 0) continue;

            HeapTupleHeaderData *tup = (HeapTupleHeaderData *)PageGetItem(page, id);
            if (!(tup->t_infomask & HEAP_XMAX_INVALID) &&
                 (tup->t_infomask & (HEAP_XMAX_COMMITTED | HEAP_XMAX_INVALID)) != HEAP_XMAX_INVALID)
                continue;

            int tup_natts = HeapTupleHeaderGetNatts(tup);
            if (tup_natts < 2) continue;

            int has_nulls = (tup->t_infomask & HEAP_HASNULL) != 0;
            uint8 *null_bits = tup->t_bits;
            char *tp = (char *)tup + tup->t_hoff;
            uintptr_t off = 0;

            Oid   reloid = 0;
            char  relname[64] = {0};
            char  relkind = 0;
            int16 relnatts = 0;
            int32 relpages = 0;
            float reltuples = 0.0f;
            int   got_name = 0;

            int ncols = (Natts_pg_class < tup_natts) ? Natts_pg_class : tup_natts;
            for (int attnum = 0; attnum < ncols; attnum++)
            {
                if (has_nulls && att_isnull(attnum, null_bits))
                    continue;
                off = att_align_nominal(off, cl_attalign[attnum]);
                char *data = tp + off;

                if (attnum == 0)  /* oid */
                    reloid = *(Oid *)data;
                else if (attnum == 1) {  /* relname */
                    memcpy(relname, data, 64);
                    relname[63] = '\0';
                    got_name = 1;
                } else if (attnum == 16)  /* relkind (0-based) */
                    relkind = *data;
                else if (attnum == 17)  /* relnatts */
                    relnatts = *(int16 *)data;
                else if (attnum == 9)   /* relpages */
                    relpages = *(int32 *)data;
                else if (attnum == 10)  /* reltuples */
                    reltuples = *(float *)data;

                off = att_addlength(off, cl_attlen[attnum], data);
            }

            if (!got_name || !relname[0]) continue;

            total++;
            switch (relkind) {
                case 'r': ntables++; break;
                case 'i': nindexes++; break;
                case 'v': nviews++; break;
                case 'S': nseqs++; break;
                case 'm': nmatviews++; break;
                case 'p': npart++; break;
                case 'f': nforeign++; break;
            }

            const char *kind_desc;
            switch (relkind) {
                case 'r': kind_desc = "表"; break;
                case 'i': kind_desc = "索引"; break;
                case 'v': kind_desc = "视图"; break;
                case 'S': kind_desc = "序列"; break;
                case 'm': kind_desc = "物化视图"; break;
                case 'p': kind_desc = "分区表"; break;
                case 'f': kind_desc = "外部表"; break;
                case 't': kind_desc = "TOAST"; break;
                case 'c': kind_desc = "复合类型"; break;
                case 'I': kind_desc = "分区索引"; break;
                default:  kind_desc = "?"; break;
            }

            char size_info[32];
            if (relpages > 0)
                snprintf(size_info, sizeof(size_info), "%d页/%.0f行", relpages, reltuples);
            else
                snprintf(size_info, sizeof(size_info), "%d页", relpages);
            if ((table_only == 1 && relkind == 'r') || table_only == 0) {
                printf("%-4d %-34s %-6u %-8s %-5d %s\n",
                    total, relname, reloid, kind_desc, relnatts, size_info);
            }
            
            if (verbose && relpages > 0) {
                printf("      [详情] relpages=%d reltuples=%.0f relnatts=%d\n",
                       relpages, reltuples, relnatts);
            }
        }
        free(page);
    }

    printf("\n共 %d 个对象: 表=%d 索引=%d 视图=%d 序列=%d",
           total, ntables, nindexes, nviews, nseqs);
    if (nmatviews) printf(" 物化视图=%d", nmatviews);
    if (npart)     printf(" 分区表=%d", npart);
    if (nforeign)  printf(" 外部表=%d", nforeign);
    printf("\n");
}

/* ================================================================
 * 读取表数据: 列定义 & 行遍历
 * ================================================================ */

#define ATTR_REL_OID      1249
#define Natts_pg_attribute 26  /* 21 定长 + 5 变长 */

/*
 * pg_attribute 定长列布局 (PG17, 0-based):
 *  0:attrelid    Oid    4,i
 *  1:attname     Name   64,c
 *  2:atttypid    Oid    4,i
 *  3:attlen      int16  2,s
 *  4:attnum      int16  2,s
 *  5:attcacheoff int32  4,i
 *  6:atttypmod   int32  4,i
 *  7:attndims    int16  2,s
 *  8:attbyval    bool   1,c
 *  9:attalign    char   1,c
 * 10:attstorage  char   1,c
 * 11:attcompression char 1,c
 * 12:attnotnull  bool   1,c
 * 13:atthasdef   bool   1,c
 * 14:atthasmissing bool 1,c
 * 15:attidentity char   1,c
 * 16:attgenerated char  1,c
 * 17:attisdropped bool  1,c
 * 18:attislocal  bool   1,c
 * 19:attinhcount int16  2,s
 * 20:attcollation Oid   4,i
 * -- CATALOG_VARLEN --
 * 21:attstattarget(int16,nul),22:attacl,23:attoptions,24:attfdwoptions,25:attmissingval
 */
static const int   at_attlen[]   = {4,64,4,2,2,4,4,2,1,1,1,1,1,1,1,1,1,1,1,2,4};
static const char  at_attalign[] = {'i','c','i','s','s','i','i','s','c','c','c','c','c','c','c','c','c','c','c','s','i'};

/* ================================================================
 * PG13 pg_attribute 定长列布局 (17 列)
 *
 *  0:attrelid      Oid    4,i
 *  1:attname       Name   64,c
 *  2:atttypid      Oid    4,i
 *  3:attstattarget int32  4,i
 *  4:attlen        int16  2,s
 *  5:attnum        int16  2,s
 *  6:attndims      int32  4,i
 *  7:attcacheoff   int32  4,i
 *  8:atttypmod     int32  4,i
 *  9:attbyval      bool   1,c
 * 10:attalign      char   1,c
 * 11:attstorage    char   1,c
 * 12:attgenerated  bool   1,c
 * 13:attisdropped  bool   1,c
 * 14:attislocal    bool   1,c
 * 15:attinhcount   int16  2,s
 * 16:attcollation  Oid    4,i
 * -- CATALOG_VARLEN --
 * 17:attacl, 18:attoptions, 19:attfdwoptions, 20:attmissingval
 * ================================================================ */
#define Natts_pg_attribute_pg13 21  /* 17 定长 + 4 变长 */
static const int   at_attlen_13[]   = {4,64,4,4,2,2,4,4,4,1,1,1,1,1,1,2,4,  -1,-1,-1,-1};
static const char  at_attalign_13[] = {'i','c','i','i','s','s','i','i','i','c','c','c','c','c','c','s','i',  'i','i','i','i'};

/*
 * PG14-15 pg_attribute 定长列布局 (18 列)
 * 比 PG13 多了 attcompression (索引 12), 其余索引 +1
 */
#define Natts_pg_attribute_pg14 22  /* 18 定长 + 4 变长 */
static const int   at_attlen_14[]   = {4,64,4,4,2,2,4,4,4,1,1,1,1,1,1,1,2,4,  -1,-1,-1,-1};
static const char  at_attalign_14[] = {'i','c','i','i','s','s','i','i','i','c','c','c','c','c','c','c','s','i',  'i','i','i','i'};

/*
 * PG16 pg_attribute 定长列布局 (22 列)
 * 比 PG14 多了 attnotnull/atthasdef/atthasmissing/attidentity (索引 13-16)
 */
#define Natts_pg_attribute_pg16 26  /* 22 定长 + 4 变长 */
static const int   at_attlen_16[]   = {4,64,4,4,2,2,4,4,4,1,1,1,1,1,1,1,1,1,1,1,2,4,  -1,-1,-1,-1};
static const char  at_attalign_16[] = {'i','c','i','i','s','s','i','i','i','c','c','c','c','c','c','c','c','c','c','c','s','i',  'i','i','i','i'};

/* 列元数据 */
typedef struct {
    char    name[64];
    Oid     typeid;
    int16   attlen;
    char    attalign;
    int     attbyval;
    int     attnum;       /* 1-based column number */
    int     isdropped;
} ColumnInfo;

#define MAX_COLUMNS 256

/*
 * 在 pg_class 中查找表，返回其 OID、relfilenode、relkind
 */
static int
find_table_info(const char *datadir, Oid dboid, const char *tabname,
                Oid *reloid_out, Oid *relfilenode_out, char *relkind_out)
{
    char subdir[32];
    snprintf(subdir, sizeof(subdir), "base/%u", dboid);

    BlockNumber npages = get_file_pages(datadir, subdir, CLASS_REL_OID);
    for (BlockNumber blk = 0; blk < npages; blk++)
    {
        char *page = read_page(datadir, subdir, CLASS_REL_OID, blk);
        if (!page) continue;

        OffsetNumber maxoff = PageGetMaxOffsetNumber(page);
        for (OffsetNumber i = 1; i <= maxoff; i++)
        {
            ItemIdData *id = PageGetItemId(page, i);
            if (id->lp_flags != LP_NORMAL || id->lp_len == 0) continue;

            HeapTupleHeaderData *tup = (HeapTupleHeaderData *)PageGetItem(page, id);
            if (!(tup->t_infomask & HEAP_XMAX_INVALID) &&
                 (tup->t_infomask & (HEAP_XMAX_COMMITTED | HEAP_XMAX_INVALID)) != HEAP_XMAX_INVALID)
                continue;

            int tup_natts = HeapTupleHeaderGetNatts(tup);
            if (tup_natts < 2) continue;
            int has_nulls = (tup->t_infomask & HEAP_HASNULL) != 0;
            uint8 *null_bits = tup->t_bits;
            char *tp = (char *)tup + tup->t_hoff;
            uintptr_t off = 0;

            Oid   found_oid = 0, found_fn = 0;
            char  found_name[64] = {0}, found_kind = 0;
            int   ncols = (Natts_pg_class < tup_natts) ? Natts_pg_class : tup_natts;

            for (int attnum = 0; attnum < ncols; attnum++)
            {
                if (has_nulls && att_isnull(attnum, null_bits)) continue;
                off = att_align_nominal(off, cl_attalign[attnum]);
                char *data = tp + off;

                if (attnum == 0) found_oid = *(Oid *)data;
                else if (attnum == 1) { memcpy(found_name, data, 64); found_name[63] = '\0'; }
                else if (attnum == 7) found_fn = *(Oid *)data;
                else if (attnum == 16) found_kind = *data;

                off = att_addlength(off, cl_attlen[attnum], data);
            }

            if (strcmp(found_name, tabname) == 0) {
                *reloid_out = found_oid;
                *relfilenode_out = (found_fn == 0) ? found_oid : found_fn;
                *relkind_out = found_kind;
                free(page);
                return 1;
            }
        }
        free(page);
    }
    return 0;
}

/*
 * 读取表的列定义 (从 pg_attribute)
 */
static int
read_columns(const char *datadir, Oid dboid, Oid reloid,
             ColumnInfo *cols, int max_cols)
{
    char subdir[32];
    snprintf(subdir, sizeof(subdir), "base/%u", dboid);

    BlockNumber npages = get_file_pages(datadir, subdir, ATTR_REL_OID);
    if (npages == 0) return 0;

    int ncols = 0;

    for (BlockNumber blk = 0; blk < npages; blk++)
    {
        char *page = read_page(datadir, subdir, ATTR_REL_OID, blk);
        if (!page) continue;

        OffsetNumber maxoff = PageGetMaxOffsetNumber(page);
        for (OffsetNumber i = 1; i <= maxoff && ncols < max_cols; i++)
        {
            ItemIdData *id = PageGetItemId(page, i);
            if (id->lp_flags != LP_NORMAL || id->lp_len == 0) continue;

            HeapTupleHeaderData *tup = (HeapTupleHeaderData *)PageGetItem(page, id);
            if (!(tup->t_infomask & HEAP_XMAX_INVALID) &&
                 (tup->t_infomask & (HEAP_XMAX_COMMITTED | HEAP_XMAX_INVALID)) != HEAP_XMAX_INVALID)
                continue;

            int tup_natts = HeapTupleHeaderGetNatts(tup);
            if (tup_natts < 2) continue;

            int has_nulls = (tup->t_infomask & HEAP_HASNULL) != 0;
            uint8 *null_bits = tup->t_bits;
            char *tp = (char *)tup + tup->t_hoff;
            uintptr_t off = 0;

            Oid   a_attrelid = 0;
            int16 a_attnum = 0;
            int   nattrs = (Natts_pg_attribute < tup_natts) ? Natts_pg_attribute : tup_natts;

            ColumnInfo ci;
            memset(&ci, 0, sizeof(ci));

            for (int attnum = 0; attnum < nattrs; attnum++)
            {
                if (has_nulls && att_isnull(attnum, null_bits)) continue;
                off = att_align_nominal(off, at_attalign[attnum]);
                char *data = tp + off;

                if (attnum == 0)      a_attrelid = *(Oid *)data;
                else if (attnum == 1) { memcpy(ci.name, data, 64); ci.name[63] = '\0'; }
                else if (attnum == 2) ci.typeid = *(Oid *)data;
                else if (attnum == 3) ci.attlen = *(int16 *)data;
                else if (attnum == 4) a_attnum = *(int16 *)data;
                else if (attnum == 8) ci.attbyval = (*(uint8 *)data) ? 1 : 0;
                else if (attnum == 9) ci.attalign = *data;
                else if (attnum == 17) ci.isdropped = (*(uint8 *)data) ? 1 : 0;

                off = att_addlength(off, at_attlen[attnum], data);
            }

            /* dump 前 1 个匹配的 pg_attribute tuple */
            static int rcdump = 0;
            if (rcdump < 1 && a_attrelid == reloid) {
                if(OPEN_DEBUG_LOG == 0){
                    fprintf(stderr, "[read_columns] 1st match: a_attnum=%d t_hoff=%d\n",
                        a_attnum, tup->t_hoff);
                    fprintf(stderr, "[read_columns] tp+00..+5f: ");
                    for (int k=0;k<96;k++) fprintf(stderr, "%02x%s", (uint8)tp[k], (k%16==15)?"\n        ":" ");
                    fprintf(stderr, "\n");
                }
                rcdump++;
            }

            if (a_attrelid == reloid && a_attnum > 0) {
                ci.attnum = a_attnum;
                cols[ncols++] = ci;
            }
        }
        free(page);
    }

    /* 按 attnum 排序 (冒泡, 列数很少) */
    for (int i = 0; i < ncols - 1; i++)
        for (int j = i + 1; j < ncols; j++)
            if (cols[i].attnum > cols[j].attnum) {
                ColumnInfo tmp = cols[i];
                cols[i] = cols[j];
                cols[j] = tmp;
            }
    return ncols;
}

/*
 * PG13 版本的 read_columns — pg_attribute 定长部分只有 17 列，
 * 与 PG17 的 21 列在索引 3~11 处布局完全不同。
 */
static int
read_columns_pg13(const char *datadir, Oid dboid, Oid reloid,
                  ColumnInfo *cols, int max_cols)
{
    char subdir[32];
    snprintf(subdir, sizeof(subdir), "base/%u", dboid);

    BlockNumber npages = get_file_pages(datadir, subdir, ATTR_REL_OID);
    if (npages == 0) return 0;

    int ncols = 0;

    for (BlockNumber blk = 0; blk < npages; blk++)
    {
        char *page = read_page(datadir, subdir, ATTR_REL_OID, blk);
        if (!page) continue;

        OffsetNumber maxoff = PageGetMaxOffsetNumber(page);
        for (OffsetNumber i = 1; i <= maxoff && ncols < max_cols; i++)
        {
            ItemIdData *id = PageGetItemId(page, i);
            if (id->lp_flags != LP_NORMAL || id->lp_len == 0) continue;

            HeapTupleHeaderData *tup = (HeapTupleHeaderData *)PageGetItem(page, id);
            if (!(tup->t_infomask & HEAP_XMAX_INVALID) &&
                 (tup->t_infomask & (HEAP_XMAX_COMMITTED | HEAP_XMAX_INVALID)) != HEAP_XMAX_INVALID)
                continue;

            int tup_natts = HeapTupleHeaderGetNatts(tup);
            if (tup_natts < 2) continue;

            int has_nulls = (tup->t_infomask & HEAP_HASNULL) != 0;
            uint8 *null_bits = tup->t_bits;
            char *tp = (char *)tup + tup->t_hoff;
            uintptr_t off = 0;

            Oid   a_attrelid = 0;
            int16 a_attnum = 0;
            int   nattrs = (Natts_pg_attribute_pg13 < tup_natts)
                         ? Natts_pg_attribute_pg13 : tup_natts;

            ColumnInfo ci;
            memset(&ci, 0, sizeof(ci));

            for (int attnum = 0; attnum < nattrs; attnum++)
            {
                if (has_nulls && att_isnull(attnum, null_bits))
                    continue;
                off = att_align_nominal(off, at_attalign_13[attnum]);
                char *data = tp + off;

                /* PG13 索引映射 */
                if (attnum == 0)        a_attrelid = *(Oid *)data;
                else if (attnum == 1) { memcpy(ci.name, data, 64); ci.name[63] = '\0'; }
                else if (attnum == 2)   ci.typeid = *(Oid *)data;
                else if (attnum == 4)   ci.attlen = *(int16 *)data;    /* PG13: attlen 在索引 4 */
                else if (attnum == 5)   a_attnum = *(int16 *)data;     /* PG13: attnum 在索引 5 */
                else if (attnum == 9)   ci.attbyval = (*(uint8 *)data) ? 1 : 0;
                else if (attnum == 11)  ci.attalign = *data;
                else if (attnum == 13)  ci.isdropped = (*(uint8 *)data) ? 1 : 0;

                off = att_addlength(off, at_attlen_13[attnum], data);
            }

            if (a_attrelid == reloid && a_attnum > 0) {
                ci.attnum = a_attnum;
                cols[ncols++] = ci;
            }
        }
        free(page);
    }

    /* 按 attnum 排序 */
    for (int i = 0; i < ncols - 1; i++)
        for (int j = i + 1; j < ncols; j++)
            if (cols[i].attnum > cols[j].attnum) {
                ColumnInfo tmp = cols[i];
                cols[i] = cols[j];
                cols[j] = tmp;
            }

    return ncols;
}

/*
 * PG14-16 版本的 read_columns
 *
 * PG14 新增 attcompression (18 定长), PG16 新增 attnotnull/atthasdef/
 * attmissing/attidentity (22 定长)。attstattarget/attcacheoff 依然存在。
 */
static int
read_columns_pg14_16(const char *datadir, Oid dboid, Oid reloid,
                     ColumnInfo *cols, int max_cols, int pg_ver)
{
    char subdir[32];
    snprintf(subdir, sizeof(subdir), "base/%u", dboid);

    BlockNumber npages = get_file_pages(datadir, subdir, ATTR_REL_OID);
    if (npages == 0) return 0;

    /* 根据版本选择 attlen/attalign 数组和总列数 */
    const int   *att_len_arr;
    const char  *att_align_arr;
    int          natts_total;
    int          idx_isdropped;

    if (pg_ver >= 16) {
        att_len_arr   = at_attlen_16;
        att_align_arr = at_attalign_16;
        natts_total   = Natts_pg_attribute_pg16;
        idx_isdropped = 18;
    } else {
        att_len_arr   = at_attlen_14;
        att_align_arr = at_attalign_14;
        natts_total   = Natts_pg_attribute_pg14;
        idx_isdropped = 14;
    }

    int ncols = 0;
    int total_tups = 0, match_reloid = 0, match_pos  = 0;

    for (BlockNumber blk = 0; blk < npages; blk++)
    {
        char *page = read_page(datadir, subdir, ATTR_REL_OID, blk);
        if (!page) continue;

        OffsetNumber maxoff = PageGetMaxOffsetNumber(page);
        for (OffsetNumber i = 1; i <= maxoff && ncols < max_cols; i++)
        {
            ItemIdData *id = PageGetItemId(page, i);
            if (id->lp_flags != LP_NORMAL || id->lp_len == 0) continue;

            HeapTupleHeaderData *tup = (HeapTupleHeaderData *)PageGetItem(page, id);
            if (!(tup->t_infomask & HEAP_XMAX_INVALID) &&
                 (tup->t_infomask & (HEAP_XMAX_COMMITTED | HEAP_XMAX_INVALID)) != HEAP_XMAX_INVALID)
                continue;

            int tup_natts = HeapTupleHeaderGetNatts(tup);
            if (tup_natts < 2) continue;

            int has_nulls = (tup->t_infomask & HEAP_HASNULL) != 0;
            uint8 *null_bits = tup->t_bits;
            char *tp = (char *)tup + tup->t_hoff;
            uintptr_t off = 0;

            Oid   a_attrelid = 0;
            int16 a_attnum = 0;
            int   nattrs = (natts_total < tup_natts) ? natts_total : tup_natts;

            ColumnInfo ci;
            memset(&ci, 0, sizeof(ci));

            for (int attnum = 0; attnum < nattrs; attnum++)
            {
                if (has_nulls && att_isnull(attnum, null_bits))
                    continue;
                off = att_align_nominal(off, att_align_arr[attnum]);
                char *data = tp + off;

                if (attnum == 0)          a_attrelid = *(Oid *)data;
                else if (attnum == 1)    { memcpy(ci.name, data, 64); ci.name[63] = '\0'; }
                else if (attnum == 2)     ci.typeid = *(Oid *)data;
                else if (attnum == 4)     ci.attlen = *(int16 *)data;    /* 索引 4 = attlen */
                else if (attnum == 5)     a_attnum = *(int16 *)data;
                else if (attnum == 9)     ci.attbyval = (*(uint8 *)data) ? 1 : 0;
                else if (attnum == 10)    ci.attalign = *data;
                else if (attnum == idx_isdropped) ci.isdropped = (*(uint8 *)data) ? 1 : 0;

                off = att_addlength(off, att_len_arr[attnum], data);
            }

            total_tups++;
            if (a_attrelid == reloid) {
                match_reloid++;
                /* 第一个匹配的 tuple: dump tp 开始的 80 字节 */
                static int dumped = 0;
                if (!dumped) {
                    dumped = 1;
                    fprintf(stderr, "[PG%d] 1st match reloid=%u: a_attnum=%d t_hoff=%d\n",
                            pg_ver, reloid, a_attnum, tup->t_hoff);
                    fprintf(stderr, "[PG%d] tp+00..+7f: ", pg_ver);
                    for (int k = 0; k < 80; k++)
                        fprintf(stderr, "%02x%s", (uint8)tp[k], (k%16==15)?"\n        ":" ");
                    fprintf(stderr, "\n");
                }
            }
            if (a_attrelid == reloid && a_attnum > 0) {
                ci.attnum = a_attnum;
                cols[ncols++] = ci;
                match_pos++;
            }
        }
        free(page);
    }

    /* 按 attnum 排序 */
    for (int i = 0; i < ncols - 1; i++)
        for (int j = i + 1; j < ncols; j++)
            if (cols[i].attnum > cols[j].attnum) {
                ColumnInfo tmp = cols[i];
                cols[i] = cols[j];
                cols[j] = tmp;
            }

    fprintf(stderr, "[PG%d] total_tups=%d match_reloid=%d match_pos=%d ncols=%d\n",
            pg_ver, total_tups, match_reloid, match_pos, ncols);
    return ncols;
}

/*
 * 将 Datum 格式化为可读字符串 (非 SQL 模式)
 */
static void
format_value(char *buf, int bufsz, const char *data, Oid typeid, int16 attlen)
{
    if (!data) { snprintf(buf, bufsz, "NULL"); return; }

    switch (typeid) {
        case 16: snprintf(buf, bufsz, "%s", *(uint8 *)data ? "t" : "f"); break;
        case 20: snprintf(buf, bufsz, "%lld", (long long)*(int64 *)data); break;
        case 21: snprintf(buf, bufsz, "%d", *(int16 *)data); break;
        case 23: snprintf(buf, bufsz, "%d", *(int32 *)data); break;
        case 26: snprintf(buf, bufsz, "%u", *(Oid *)data); break;
        case 700: snprintf(buf, bufsz, "%.6g", (double)*(float *)data); break;
        case 701: snprintf(buf, bufsz, "%.6g", *(double *)data); break;
        case 19: snprintf(buf, bufsz, "%.64s", data); break;
        case 25: case 1042: case 1043: {
            int is_1b = (*(uint8 *)data & 0x01);
            int total = is_1b ? (*(uint8 *)data >> 1) : (*(uint32 *)data >> 2);
            int len = total - (is_1b ? 1 : 4);
            int cp = (len > bufsz - 1) ? bufsz - 1 : len;
            memcpy(buf, data + (is_1b ? 1 : 4), cp); buf[cp] = '\0';
            break;
        }
        case 1082: snprintf(buf, bufsz, "%d", *(int32 *)data); break;
        case 1114: snprintf(buf, bufsz, "%lld", (long long)*(int64 *)data); break;
        default: {
            int show = (attlen > 0 && attlen < 16) ? attlen : 16;
            int pos = 0;
            for (int k = 0; k < show && pos < bufsz - 4; k++)
                pos += snprintf(buf + pos, bufsz - pos, "%02x ", (uint8)data[k]);
            if (attlen == -1 && pos < bufsz - 4) snprintf(buf + pos, bufsz - pos, "...");
            break;
        }
    }
}

/*
 * 类型 OID -> SQL 类型名
 */
static const char *
type_name(Oid typeid, int16 atttypmod)
{
    switch (typeid) {
        case 16:  return "boolean";
        case 19:  return "name";
        case 20:  return "bigint";
        case 21:  return "smallint";
        case 23:  return "integer";
        case 25:  return "text";
        case 26:  return "oid";
        case 700: return "real";
        case 701: return "double precision";
        case 1042: return "character";
        case 1043: {
            if (atttypmod > 4) {
                static char buf[32];
                snprintf(buf, sizeof(buf), "character varying(%d)", atttypmod - 4);
                return buf;
            }
            return "character varying";
        }
        case 1082: return "date";
        case 1083: return "time";
        case 1114: return "timestamp";
        case 1184: return "timestamptz";
        case 1186: return "interval";
        case 1700: return "numeric";
        case 2950: return "uuid";
        default: {
            static char buf[20];
            snprintf(buf, sizeof(buf), "oid(%u)", typeid);
            return buf;
        }
    }
}

/*
 * 将 Datum 格式化为 SQL 字面值（加引号、转义）
 */
static void
format_sql_value(char *buf, int bufsz, const char *data, Oid typeid, int16 attlen)
{
    if (!data) { snprintf(buf, bufsz, "NULL"); return; }

    switch (typeid) {
        case 16: /* boolean */
            snprintf(buf, bufsz, "%s", *(uint8 *)data ? "TRUE" : "FALSE");
            break;
        case 20: /* int8 */
            snprintf(buf, bufsz, "%lld", (long long)*(int64 *)data);
            break;
        case 21: /* int2 */
            snprintf(buf, bufsz, "%d", *(int16 *)data);
            break;
        case 23: /* int4 */
            snprintf(buf, bufsz, "%d", *(int32 *)data);
            break;
        case 26: /* oid */
            snprintf(buf, bufsz, "%u", *(Oid *)data);
            break;
        case 700: /* float4 */
            snprintf(buf, bufsz, "%.6g", (double)*(float *)data);
            break;
        case 701: /* float8 */
            snprintf(buf, bufsz, "%.6g", *(double *)data);
            break;
        case 19: { /* name — 定长64字节，去尾部\0 */
            int len = 64;
            while (len > 0 && data[len-1] == '\0') len--;
            buf[0] = 39; int pos = 1;
            for (int k = 0; k < len && pos < bufsz - 3; k++) {
                if (data[k] == 39) { buf[pos++] = 39; buf[pos++] = 39; }
                else buf[pos++] = data[k];
            }
            buf[pos++] = 39; buf[pos] = '\0';
            break;
        }
        case 25:  /* text */
        case 1042: /* bpchar */
        case 1043: /* varchar */
            {
                int is_1b = (*(uint8 *)data & 0x01);
                int total_len = is_1b ? (*(uint8 *)data >> 1) : (*(uint32 *)data >> 2);
                int data_len = total_len - (is_1b ? 1 : 4);
                const char *src = data + (is_1b ? 1 : 4);
                buf[0] = 39; int pos = 1;
                int limit = (data_len > (bufsz - 4) / 2) ? (bufsz - 4) / 2 : data_len;
                for (int k = 0; k < limit && pos < bufsz - 3; k++) {
                    if (src[k] == 39) { buf[pos++] = 39; buf[pos++] = 39; }
                    else if (src[k] == 92) { buf[pos++] = 92; buf[pos++] = 92; }
                    else buf[pos++] = src[k];
                }
                buf[pos++] = 39; buf[pos] = '\0';
            }
            break;
        case 1082: /* date: days since 2000-01-01 */
            {
                int days = *(int32 *)data;
                snprintf(buf, bufsz, "'%d-days-from-2000'::date", days);
            }
            break;
        case 1114: /* timestamp */
            snprintf(buf, bufsz, "'%lld-us'::timestamp", (long long)*(int64 *)data);
            break;
        default:
            {
                int show = (attlen > 0 && attlen < 32) ? attlen : 32;
                int pos = 0;
                buf[pos++] = 39; /* ' */
                for (int k = 0; k < show && pos < bufsz - 3; k++)
                    pos += snprintf(buf + pos, bufsz - pos, "%02x", (uint8)data[k]);
                if (pos < bufsz - 2) { buf[pos++] = 39; buf[pos] = '\0'; }
            }
            break;
    }
}

/*
 * 打印 CREATE TABLE 语句
 */
static void
print_create_table(const char *tabname, ColumnInfo *cols, int ncols)
{
    printf("CREATE TABLE %s (\n", tabname);
    int printed = 0;
    for (int i = 0; i < ncols; i++) {
        if (cols[i].isdropped) continue;
        printf("    %s %s", cols[i].name, type_name(cols[i].typeid, 0));
        printed++;
        if (i < ncols - 1) {
            /* 检查后面是否还有非 dropped 列 */
            int more = 0;
            for (int j = i + 1; j < ncols; j++)
                if (!cols[j].isdropped) { more = 1; break; }
            if (more) printf(",");
        }
        printf("\n");
    }
    printf(");");
    if (printed == 0) printf(" -- 无可见列");
    printf("\n");
}

/*
 * 生成 INSERT 语句 — 读取并打印表的所有行
 */
static void
read_table_rows(const char *datadir, Oid dboid, Oid relfilenode, char relkind,
                const char *tabname, ColumnInfo *cols, int ncols, int sql_mode)
{
    char subdir[32];
    snprintf(subdir, sizeof(subdir), "base/%u", dboid);

    if (relkind != 'r' && relkind != 'p') {
        printf("relkind='%c': 仅支持普通表(r)和分区表(p)\n", relkind);
        return;
    }

    /* 构建列名列表 (跳过 dropped) */
    char col_list[4096] = {0};
    int col_pos = 0, visible = 0;
    for (int i = 0; i < ncols; i++) {
        if (cols[i].isdropped) continue;
        if (visible > 0) col_pos += snprintf(col_list + col_pos,
                                              sizeof(col_list) - col_pos, ", ");
        col_pos += snprintf(col_list + col_pos, sizeof(col_list) - col_pos,
                           "%s", cols[i].name);
        visible++;
    }
    if (visible == 0) { printf("无可见列\n"); return; }

    BlockNumber npages = get_file_pages(datadir, subdir, relfilenode);
    int total_rows = 0, dead_rows = 0;

    /* SQL 模式: 先输出 CREATE TABLE */
    if (sql_mode) {
        printf("--\n-- 表: %s (relfilenode=%u, 页数=%u)\n--\n\n",
               tabname, relfilenode, npages);
        print_create_table(tabname, cols, ncols);
        printf("\n");
    } else {
        /* 非 SQL 模式: 打印列头 */
        int first = 1;
        for (int c = 0; c < ncols; c++) {
            if (cols[c].isdropped) continue;
            printf("%-*s ", first ? 24 : 16, cols[c].name);
            first = 0;
        }
        printf("\n");
        for (int c = 0; c < ncols; c++) {
            if (cols[c].isdropped) continue;
            printf("%-*s ", 16, "----------------");
        }
        printf("\n");
    }

    for (BlockNumber blk = 0; blk < npages; blk++)
    {
        char *page = read_page(datadir, subdir, relfilenode, blk);
        if (!page) continue;

        OffsetNumber maxoff = PageGetMaxOffsetNumber(page);
        for (OffsetNumber i = 1; i <= maxoff; i++)
        {
            ItemIdData *id = PageGetItemId(page, i);
            if (id->lp_flags != LP_NORMAL || id->lp_len == 0) {
                if (id->lp_flags == LP_DEAD) dead_rows++;
                continue;
            }

            HeapTupleHeaderData *tup = (HeapTupleHeaderData *)PageGetItem(page, id);

            if (!(tup->t_infomask & HEAP_XMAX_INVALID)) { dead_rows++; continue; }

            int tup_natts = HeapTupleHeaderGetNatts(tup);
            int has_nulls = (tup->t_infomask & HEAP_HASNULL) != 0;
            uint8 *null_bits = tup->t_bits;
            char *tp = (char *)tup + tup->t_hoff;
            uintptr_t off = 0;

            total_rows++;

            /* 第一行数据: 打印每列的偏移和原始字节  */
            if (OPEN_DEBUG_LOG == 0 && total_rows == 1) {
                fprintf(stderr, "[DEBUG] row 1: tup_natts=%d has_nulls=%d t_hoff=%d\n",
                        tup_natts, has_nulls, tup->t_hoff);
                if (has_nulls) {
                    int nb = (tup_natts + 7) / 8;
                    fprintf(stderr, "[DEBUG] null_bits(%d): ", nb);
                    for (int k = 0; k < nb && k < 8; k++) fprintf(stderr, "%02x ", null_bits[k]);
                    fprintf(stderr, "\n");
                }
                /* dump tp+0 到 tp+79  */ 
                fprintf(stderr, "[DEBUG] tp+30..+6f: ");
                for (int k=48;k<112;k++) fprintf(stderr, "%02x%s", (uint8)tp[k], (k%16==15)?"\n        ":" ");
                fprintf(stderr, "\n");
                uintptr_t dbg_off = 0;
                for (int dc = 0; dc < ncols; dc++) {
                    if (cols[dc].isdropped) {
                        if (dc < tup_natts && !(has_nulls && att_isnull(dc, null_bits))) {
                            char dbg_al2 = (cols[dc].attlen == -1) ? 'c' : cols[dc].attalign;
                            dbg_off = att_addlength(
                                att_align_nominal(dbg_off, dbg_al2),
                                cols[dc].attlen, tp + dbg_off);
                        }
                        continue;
                    }
                    if (dc >= tup_natts || (has_nulls && att_isnull(dc, null_bits))) {
                        fprintf(stderr, "[DEBUG] col[%d]=%s: NULL\n", dc, cols[dc].name);
                        continue;
                    }
                    char dbg_al = (cols[dc].attlen == -1) ? 'c' : cols[dc].attalign;
                    dbg_off = att_align_nominal(dbg_off, dbg_al);
                    char *dbg_data = tp + dbg_off;
                    fprintf(stderr, "[DEBUG] col[%d]=%s: off=%zu align=%c typeid=%u attlen=%d "
                            "raw=%02x %02x %02x %02x %02x %02x %02x %02x\n",
                            dc, cols[dc].name, (size_t)dbg_off, cols[dc].attalign,
                            cols[dc].typeid, cols[dc].attlen,
                            (uint8)dbg_data[0], (uint8)dbg_data[1],
                            (uint8)dbg_data[2], (uint8)dbg_data[3],
                            (uint8)dbg_data[4], (uint8)dbg_data[5],
                            (uint8)dbg_data[6], (uint8)dbg_data[7]);
                    dbg_off = att_addlength(dbg_off, cols[dc].attlen, dbg_data);
                }
            }

            if (sql_mode) {
                /* --- SQL INSERT 模式 --- */
                char val_list[8192] = {0};
                int val_pos = 0, first_val = 1;

                for (int c = 0; c < ncols; c++) {
                    if (cols[c].isdropped) {
                        if (c < tup_natts && !(has_nulls && att_isnull(c, null_bits))) {
                            char al = (cols[c].attlen == -1) ? 'c' : cols[c].attalign;
                            off = att_align_nominal(off, al);
                            off = att_addlength(off, cols[c].attlen, tp + off);
                        }
                        continue;
                    }
                    if (!first_val) val_pos += snprintf(val_list + val_pos,
                        sizeof(val_list) - val_pos, ", ");
                    first_val = 0;

                    if (c >= tup_natts || (has_nulls && att_isnull(c, null_bits))) {
                        val_pos += snprintf(val_list + val_pos,
                                            sizeof(val_list) - val_pos, "NULL");
                        continue;
                    }
                    char al = (cols[c].attlen == -1) ? 'c' : cols[c].attalign;
                    off = att_align_nominal(off, al);
                    char *data = tp + off;
                    char val[256];
                    format_sql_value(val, sizeof(val), data, cols[c].typeid, cols[c].attlen);
                    val_pos += snprintf(val_list + val_pos, sizeof(val_list) - val_pos, "%s", val);
                    off = att_addlength(off, cols[c].attlen, data);
                }
                printf("INSERT INTO %s (%s) VALUES (%s);\n", tabname, col_list, val_list);
            } else {
                /* --- 原始列输出模式 --- */
                int first_col = 1;
                for (int c = 0; c < ncols; c++) {
                    if (c >= tup_natts || (has_nulls && att_isnull(c, null_bits))) {
                        if (!cols[c].isdropped) {
                            printf("%-*s ", first_col ? 24 : 16, "NULL");
                            first_col = 0;
                        }
                        continue;
                    }
                    char al2 = (cols[c].attlen == -1) ? 'c' : cols[c].attalign;
                    off = att_align_nominal(off, al2);
                    char *data = tp + off;

                    if (cols[c].isdropped) {
                        off = att_addlength(off, cols[c].attlen, data);
                        continue;
                    }
                    char val[64];
                    format_value(val, sizeof(val), data, cols[c].typeid, cols[c].attlen);
                    printf("%-*s ", first_col ? 24 : 16, val);
                    first_col = 0;
                    off = att_addlength(off, cols[c].attlen, data);
                }
                printf("\n");
            }
        }
        free(page);
    }

    if (sql_mode)
        printf("\n-- 共 %d 行 (跳过 %d 死元组)\n", total_rows, dead_rows);
    else
        printf("\n共 %d 页, %d 行, %d 死元组\n", npages, total_rows, dead_rows);
}

/* ================================================================
 * 主入口
 * ================================================================ */

static const char *
encoding_name(int32 enc)
{
    switch (enc) {
        case 0:  return "SQL_ASCII";
        case 6:  return "UTF8";
        case 7:  return "LATIN1";
        case 8:  return "LATIN2";
        case 9:  return "LATIN3";
        case 10: return "LATIN4";
        case 11: return "LATIN5";
        case 12: return "LATIN6";
        case 13: return "LATIN7";
        case 14: return "LATIN8";
        case 15: return "LATIN9";
        case 16: return "LATIN10";
        case 20: return "UTF8";
        case 21: return "WIN866";
        case 22: return "WIN1251";
        case 23: return "WIN1252";
        case 24: return "WIN1253";
        case 25: return "WIN1254";
        case 26: return "WIN1255";
        case 27: return "WIN1256";
        case 28: return "WIN1257";
        case 29: return "WIN1258";
        case 30: return "EUC_CN";
        case 31: return "EUC_KR";
        case 32: return "EUC_TW";
        case 33: return "EUC_JP";
        case 34: return "EUC_JIS_2004";
        case 35: return "GBK";
        case 36: return "GB18030";
        case 37: return "SHIFT_JIS_2004";
        case 38: return "ISO_8859_5";
        case 39: return "ISO_8859_6";
        case 40: return "ISO_8859_7";
        case 41: return "ISO_8859_8";
        case 42: return "BIG5";
        case 43: return "KOI8R";
        case 44: return "KOI8U";
        default: return "?";
    }
}
static void
print_usage(const char *prog)
{
    printf("用法:\n");
    printf("  %s <PGDATA>                           列出所有数据库\n", prog);
    printf("  %s <PGDATA> <数据库名>                列出库中所有表\n", prog);
    printf("  %s <PGDATA> <数据库名> <表名>         读取表数据行\n", prog);
    printf("  %s <PGDATA> <数据库名> <表名> -sql    输出为 SQL 语句\n", prog);
    printf("  %s <PGDATA> -v <库名>                 显示详情\n\n", prog);
    printf("从 PostgreSQL 数据目录直接读取信息的独立工具。\n");
}

int
main(int argc, char *argv[])
{
    const char *datadir = NULL;
    const char *dbname  = NULL;
    const char *tabname = NULL;
    int verbose = 0;
    int sql_mode = 0;
    int table_only = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "--verbose") == 0 || strcmp(argv[i], "-v") == 0) {
            verbose = 1;
        } else if (strcmp(argv[i], "-sql") == 0 || strcmp(argv[i], "--sql") == 0) {
            sql_mode = 1;
        } else if (strcmp(argv[i], "-table") == 0 || strcmp(argv[i], "--table") == 0) {
            table_only = 1;
        } else if (!datadir) {
            datadir = argv[i];
        } else if (!dbname) {
            dbname = argv[i];
        } else if (!tabname) {
            tabname = argv[i];
        }
    }

    if (!datadir) {
        fprintf(stderr, "错误: 请指定 PGDATA 路径\n");
        print_usage(argv[0]);
        return 1;
    }

    printf("数据目录: %s\n", datadir);
    if (!validate_pgdata(datadir))
        return 1;

    /* PG_VERSION */
    char version_path[1024];
    snprintf(version_path, sizeof(version_path), "%s/%s", datadir, PG_VERSION_FILE);
    FILE *vfp = fopen(version_path, "r");
    char version_str[32] = {0};
    if (vfp) {
        fgets(version_str, sizeof(version_str), vfp);
        fclose(vfp);
        version_str[strcspn(version_str, "\n")] = '\0';
    }
    printf("PG 版本: %s\n\n", version_str);

    /* 模式3: 读取表数据行 */
    if (dbname && tabname) {
        Oid dboid = find_database_oid(datadir, dbname);
        if (dboid == 0) {
            fprintf(stderr, "错误: 未找到数据库 '%s'\n", dbname);
            return 1;
        }

        Oid reloid, relfilenode;
        char relkind;
        if (!find_table_info(datadir, dboid, tabname,
                             &reloid, &relfilenode, &relkind)) {
            fprintf(stderr, "错误: 未找到表 '%s'\n", tabname);
            return 1;
        }

        printf("=== 表 '%s' (OID=%u, relfilenode=%u, relkind=%c) ===\n\n",
               tabname, reloid, relfilenode, relkind);

        ColumnInfo cols[MAX_COLUMNS];
        /* 根据 PG 大版本选择正确的 pg_attribute 解析函数 */
        int pg_ver = atoi(version_str);
        int ncols;
        if (pg_ver >= 16)
            ncols = read_columns(datadir, dboid, reloid, cols, MAX_COLUMNS);
        else if (pg_ver >= 14)
            ncols = read_columns_pg14_16(datadir, dboid, reloid, cols, MAX_COLUMNS, pg_ver);
        else
            ncols = read_columns_pg13(datadir, dboid, reloid, cols, MAX_COLUMNS);
        if (ncols == 0) {
            printf("未找到列定义\n");
            return 1;
        }
        printf("列数: %d\n\n", ncols);

        read_table_rows(datadir, dboid, relfilenode, relkind,
                        tabname, cols, ncols, sql_mode);
        return 0;
    }

    /* 模式2: 列出指定数据库中的表 */
    if (dbname) {
        Oid dboid = find_database_oid(datadir, dbname);
        if (dboid == 0) {
            fprintf(stderr, "错误: 未找到数据库 '%s'\n", dbname);
            return 1;
        }
        printf("=== 数据库 '%s' (OID=%u) 中的对象 ===\n\n", dbname, dboid);
        list_tables(datadir, dboid, verbose,table_only);
        return 0;
    }

    /* 模式1: 列出所有数据库 */
    printf("=== 数据库列表 (从 pg_database 系统表读取) ===\n\n");
    DBEntry *entries = calloc(MAX_DATABASES, sizeof(DBEntry));
    if (!entries) {
        fprintf(stderr, "内存分配失败\n");
        return 1;
    }

    int dbcount = parse_pg_database(datadir, entries, MAX_DATABASES);

    if (dbcount == 0) {
        printf("未能从 pg_database 读取到数据库。\n");
        printf("尝试 base/ 目录扫描...\n");
        int count = count_databases_simple(datadir);
        printf("base/ 下找到 %d 个子目录\n", count);
        free(entries);
        return count > 0 ? 0 : 1;
    }

    printf("%-4s %-22s %-6s %-12s %s\n",
           "序号", "数据库名", "OID", "编码", "属性");
    printf("%-4s %-22s %-6s %-12s %s\n",
           "----", "----------------------", "------", "------------", "----");

    for (int i = 0; i < dbcount; i++) {
        DBEntry *e = &entries[i];
        char attr[64] = {0};
        int pos = 0;
        if (e->istemplate) pos += snprintf(attr + pos, sizeof(attr) - pos, " 模板");
        if (!e->allowconn) pos += snprintf(attr + pos, sizeof(attr) - pos, " 禁止连接");
        if (e->connlimit != -1) pos += snprintf(attr + pos, sizeof(attr) - pos, " 连接数限制:%d", e->connlimit);
        if (pos == 0) snprintf(attr, sizeof(attr), "普通");

        printf("%-4d %-22s %-6u %-12s %s\n",
               i + 1, e->name, e->oid, encoding_name(e->encoding), attr);
    }

    printf("\n共 %d 个数据库\n", dbcount);

    if (verbose) {
        printf("\n=== 详细信息 (Locale) ===\n");
        for (int i = 0; i < dbcount; i++) {
            DBEntry *e = &entries[i];
            printf("\n[%d] %s (OID=%u)\n", i + 1, e->name, e->oid);
            printf("    编码:       %s (%d)\n", encoding_name(e->encoding), e->encoding);
            printf("    LC_COLLATE: %s\n", e->collate[0] ? e->collate : "(未设置)");
            printf("    LC_CTYPE:   %s\n", e->ctype[0] ? e->ctype : "(未设置)");
            printf("    属主:       OID=%u\n", e->owner);
            printf("    表空间:     OID=%u\n", e->tablespace);
            printf("    允许连接:   %s\n", e->allowconn ? "是" : "否");
            printf("    模板:       %s\n", e->istemplate ? "是" : "否");
            printf("    连接数限制: %d\n", e->connlimit);
        }
    }

    free(entries);
    return 0;
}
