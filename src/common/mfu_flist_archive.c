
/**
 * @file dtar.c - parallel tar main file
 *
 * @author - Feiyi Wang
 *
 *
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#define _LARGEFILE64_SOURCE

#include <errno.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <mpi.h>
#include <libcircle.h>
#include <archive.h>
#include <archive_entry.h>
#include <string.h>
#include <getopt.h>

#include "mfu.h"

/* operation types */
typedef enum {
    COPY_DATA
} DTAR_operation_code_t;

/* struct to define a decoded work operation from libcircle */
typedef struct {
    uint64_t file_size;
    uint64_t chunk_index;
    uint64_t offset;
    DTAR_operation_code_t code;
    char* operand;
} DTAR_operation_t;

typedef struct {
    const char* name; /* file name of archive */
    int fd;           /* file descriptor of archive file */
    int flags;        /* flags used to open archive file */
    void* buf;        /* memory buffer in which to encode entry headers */
    size_t bufsize;   /* size of memory buffer in bytes */
} DTAR_writer_t;

DTAR_writer_t DTAR_writer;
mfu_flist DTAR_flist;
uint64_t* DTAR_offsets        = NULL; /* byte offset into archive for each entry in our list */
uint64_t* DTAR_header_sizes   = NULL; /* byte size of header for each entry in our list */
static void* DTAR_iobuf       = NULL; /* temporary buffer for reading/writing files */
mfu_archive_opts_t* DTAR_opts = NULL; /* pointer to archive options */

static void DTAR_abort(int code)
{
    MPI_Abort(MPI_COMM_WORLD, code);
    exit(code);
}

static void DTAR_exit(int code)
{
    mfu_finalize();
    MPI_Finalize();
    exit(code);
}

/****************************************
 * Global variable used for extraction progress messages
 ***************************************/

mfu_progress* extract_prog = NULL;

#define REDUCE_BYTES (0)
#define REDUCE_ITEMS (1)
static uint64_t reduce_buf[2];

/****************************************
 * Global counter and callbacks for LIBCIRCLE reductions
 ***************************************/

/* holds total item count and byte count for reduction */
uint64_t DTAR_total_items = 0;
uint64_t DTAR_total_bytes = 0;

static double   reduce_start;
static uint64_t reduce_bytes;

static void reduce_init(void)
{
    CIRCLE_reduce(&reduce_bytes, sizeof(uint64_t));
}

static void reduce_exec(const void* buf1, size_t size1, const void* buf2, size_t size2)
{
    const uint64_t* a = (const uint64_t*) buf1;
    const uint64_t* b = (const uint64_t*) buf2;
    uint64_t val = a[0] + b[0];
    CIRCLE_reduce(&val, sizeof(uint64_t));
}

static void reduce_fini(const void* buf, size_t size)
{
    /* get result of reduction */
    const uint64_t* a = (const uint64_t*) buf;
    unsigned long long val = (unsigned long long) a[0];

    /* get current time */
    double now = MPI_Wtime();

    /* compute walk rate */
    double rate = 0.0;
    double secs = now - reduce_start;
    if (secs > 0.0) {
        rate = (double)val / secs;
    }

    /* convert total bytes to units */
    double val_tmp;
    const char* val_units;
    mfu_format_bytes(val, &val_tmp, &val_units);

    /* convert bandwidth to units */
    double rate_tmp;
    const char* rate_units;
    mfu_format_bw(rate, &rate_tmp, &rate_units);

    /* compute percentage done */
    double percent = 0.0;
    if (DTAR_total_bytes > 0) {
        percent = (double)val * 100.0 / (double)DTAR_total_bytes;
    }

    /* estimate seconds remaining */
    double secs_remaining = 0.0;
    if (rate > 0.0) {
        secs_remaining = (double)(DTAR_total_bytes - (uint64_t)val) / rate;
    }

    /* print status to stdout */
    MFU_LOG(MFU_LOG_INFO, "Tarred %.3lf %s (%.0f\%) in %.3lf secs (%.3lf %s) %.0f secs left ...",
        val_tmp, val_units, percent, secs, rate_tmp, rate_units, secs_remaining
    );
}

/* given an item name, determine which source path this item
 * is contained within, extract directory components from source
 * path to this item and then prepend destination prefix. */
char* mfu_param_path_relative(
    const char* name,
    const mfu_param_path* cwdpath)
{
    /* create path of item */
    mfu_path* item = mfu_path_from_str(name);

    /* get current working directory */
    mfu_path* cwd = mfu_path_from_str(cwdpath->path);

    /* get relative path from current working dir to item */
    mfu_path* rel = mfu_path_relative(cwd, item);

    /* convert to a NUL-terminated string */
    char* dest = mfu_path_strdup(rel);

    /* free our temporary paths */
    mfu_path_delete(&rel);
    mfu_path_delete(&cwd);
    mfu_path_delete(&item);

    return dest;
}

/* given an entry in the flist, construct and encode its tar header
 * in the provided buffer, return number of bytes consumed in outsize */
static int encode_header(
    mfu_flist flist,               /* list of items */
    uint64_t idx,                  /* index of list item to be encoded */
    const mfu_param_path* cwdpath, /* current working dir to compute relative path to item */
    void* buf,                     /* buffer in which to store encoded header */
    size_t bufsize,                /* size of input buffer */
    size_t* outsize)               /* number of bytes consumed to encode header */
{
    /* assume we'll succeed */
    int rc = MFU_SUCCESS;

    /* allocate and entry for this item */
    struct archive_entry* entry = archive_entry_new();

    /* get file name for this item */
    const char* fname = mfu_flist_file_get_name(flist, idx);

    /* compute relative path to item from current working dir */
    const char* relname = mfu_param_path_relative(fname, cwdpath);
    archive_entry_copy_pathname(entry, relname);
    mfu_free(&relname);

    if (DTAR_opts->preserve) {
        struct archive* source = archive_read_disk_new();
        archive_read_disk_set_standard_lookup(source);
        int fd = mfu_open(fname, O_RDONLY);
        if (archive_read_disk_entry_from_file(source, entry, fd, NULL) != ARCHIVE_OK) {
            MFU_LOG(MFU_LOG_ERR, "archive_read_disk_entry_from_file(): %s",
                archive_error_string(source)
            );
            rc = MFU_FAILURE;
        }
        archive_read_free(source);
        mfu_close(fname, fd);
    } else {
        /* TODO: read stat info from mfu_flist */
        struct stat stbuf;
        mfu_lstat(fname, &stbuf);
        archive_entry_copy_stat(entry, &stbuf);

        /* set user name of owner */
        const char* uname = mfu_flist_file_get_username(flist, idx);
        archive_entry_set_uname(entry, uname);

        /* set group name */
        const char* gname = mfu_flist_file_get_groupname(flist, idx);
        archive_entry_set_gname(entry, gname);

        /* if entry is a symlink, copy its target */
        mfu_filetype type = mfu_flist_file_get_type(flist, idx);
        if (type == MFU_TYPE_LINK) {
            char target[PATH_MAX + 1];
            size_t bufsize = sizeof(target) - 1;
            ssize_t readlink_rc = mfu_readlink(fname, target, bufsize);
            if(readlink_rc != -1) {
                if (readlink_rc < (ssize_t)bufsize) {
                    /* null terminate the link */
                    target[readlink_rc] = '\0';
                    archive_entry_copy_symlink(entry, target);
                } else {
                    MFU_LOG(MFU_LOG_ERR, "Link target of `%s' exceeds buffer size %llu",
                        fname, bufsize
                    );
                    rc = MFU_FAILURE;
                }
            } else {
                MFU_LOG(MFU_LOG_ERR, "Failed to read link `%s' readlink() (errno=%d %s)",
                    fname, errno, strerror(errno)
                );
                rc = MFU_FAILURE;
            }
        }
    }

    /* write entry info to archive */
    struct archive* dest = archive_write_new();
    archive_write_set_format_pax(dest);

    /* don't buffer data, write everything directly to output (file or memory) */
    archive_write_set_bytes_per_block(dest, 0);

    /* encode entry into user's buffer */
    size_t used = 0;
    if (archive_write_open_memory(dest, buf, bufsize, &used) != ARCHIVE_OK) {
        MFU_LOG(MFU_LOG_ERR, "archive_write_open_memory(): %s",
            archive_error_string(dest)
        );
        rc = MFU_FAILURE;
    }

    /* write header for this item */
    if (archive_write_header(dest, entry) != ARCHIVE_OK) {
        MFU_LOG(MFU_LOG_ERR, "archive_write_header(): %s",
            archive_error_string(dest)
        );
        rc = MFU_FAILURE;
    }

    archive_entry_free(entry);

    /* at this point, used tells us the size of the header for this item */

    /* TODO: Seems to be a bug here potentially leading to corrupted
     * archive files.  archive_write_free also writes two blocks of
     * NULL bytes at the end of an archive file, however, each rank
     * will have a different view of the length of the file, so one
     * rank may write its NULL blocks over top of the actual data
     * written by another rank */

    /* mark the archive as failed, so that we skip trying to write bytes
     * that would correspond to file data when we call free, this way we
     * still free data structures that was allocated */
    archive_write_fail(dest);
    archive_write_free(dest);

    /* output size of header */
    *outsize = used;

    /* return size of header for this entry */
    return rc;
}

/* write header for specified item in flist to archive file */
static void DTAR_write_header(
    mfu_flist flist,               /* flist holding item for which to write header */
    uint64_t idx,                  /* index of item in flist */
    const mfu_param_path* cwdpath, /* current working dir, store relative path to item from cwd */
    uint64_t offset)               /* byte offset in archive at which to write header */
{
    /* encode header for this entry in our buffer */
    size_t header_size;
    encode_header(flist, idx, cwdpath,
        DTAR_writer.buf, DTAR_writer.bufsize, &header_size
    );

    /* seek to offset in tar archive for this file */
    mfu_lseek(DTAR_writer.name, DTAR_writer.fd, offset, SEEK_SET);
    mfu_write(DTAR_writer.name, DTAR_writer.fd, DTAR_writer.buf, header_size);

    return;
}

static char* DTAR_encode_operation(
    DTAR_operation_code_t code,
    const char* operand,
    uint64_t fsize,
    uint64_t chunk_idx,
    uint64_t offset)
{
    size_t opsize = (size_t) CIRCLE_MAX_STRING_LEN;
    char* op = (char*) MFU_MALLOC(opsize);
    size_t len = strlen(operand);

    int written = snprintf(op, opsize,
                           "%" PRIu64 ":%" PRIu64 ":%" PRIu64 ":%d:%d:%s",
                           fsize, chunk_idx, offset, code, (int) len, operand);

    if (written >= opsize) {
        MFU_LOG(MFU_LOG_ERR, "Exceed libcirlce message size");
        DTAR_abort(EXIT_FAILURE);
    }

    return op;
}

static DTAR_operation_t* DTAR_decode_operation(char* op)
{
    DTAR_operation_t* ret = (DTAR_operation_t*) MFU_MALLOC(sizeof(DTAR_operation_t));

    if (sscanf(strtok(op, ":"), "%" SCNu64, &(ret->file_size)) != 1) {
        MFU_LOG(MFU_LOG_ERR, "Could not decode file size attribute.");
        DTAR_abort(EXIT_FAILURE);
    }

    if (sscanf(strtok(NULL, ":"), "%" SCNu64, &(ret->chunk_index)) != 1) {
        MFU_LOG(MFU_LOG_ERR, "Could not decode chunk index attribute.");
        DTAR_abort(EXIT_FAILURE);
    }

    if (sscanf(strtok(NULL, ":"), "%" SCNu64, &(ret->offset)) != 1) {
        MFU_LOG(MFU_LOG_ERR, "Could not decode source base offset attribute.");
        DTAR_abort(EXIT_FAILURE);
    }

    if (sscanf(strtok(NULL, ":"), "%d", (int*) & (ret->code)) != 1) {
        MFU_LOG(MFU_LOG_ERR, "Could not decode stage code attribute.");
        DTAR_abort(EXIT_FAILURE);
    }

    /* get number of characters in operand string */
    int op_len;
    char* str = strtok(NULL, ":");
    if (sscanf(str, "%d", &op_len) != 1) {
        MFU_LOG(MFU_LOG_ERR, "Could not decode operand string length.");
        DTAR_abort(EXIT_FAILURE);
    }

    /* skip over digits and trailing ':' to get pointer to operand */
    char* operand = str + strlen(str) + 1;
    operand[op_len] = '\0';
    ret->operand = operand;

    return ret;
}

static void DTAR_enqueue_copy(CIRCLE_handle* handle)
{
    uint64_t listsize = mfu_flist_size(DTAR_flist);
    for (uint64_t idx = 0; idx < listsize; idx++) {
        /* add copy work only for files */
        mfu_filetype type = mfu_flist_file_get_type(DTAR_flist, idx);
        if (type == MFU_TYPE_FILE) {
            /* get name and size of file */
            const char* name = mfu_flist_file_get_name(DTAR_flist, idx);
            uint64_t size = mfu_flist_file_get_size(DTAR_flist, idx);

            /* compute offset for first byte of file content */
            uint64_t dataoffset = DTAR_offsets[idx] + DTAR_header_sizes[idx];

            /* compute number of chunks */
            uint64_t num_chunks = size / DTAR_opts->chunk_size;
            for (uint64_t chunk_idx = 0; chunk_idx < num_chunks; chunk_idx++) {
                char* newop = DTAR_encode_operation(
                                  COPY_DATA, name, size, chunk_idx, dataoffset);
                handle->enqueue(newop);
                mfu_free(&newop);
            }

            /* create copy work for possibly last item */
            if (num_chunks * DTAR_opts->chunk_size < size || num_chunks == 0) {
                char* newop = DTAR_encode_operation(
                                  COPY_DATA, name, size, num_chunks, dataoffset);
                handle->enqueue(newop);
                mfu_free(&newop);
            }
        }
    }
}

static void DTAR_perform_copy(CIRCLE_handle* handle)
{
    char opstr[CIRCLE_MAX_STRING_LEN];
    handle->dequeue(opstr);
    DTAR_operation_t* op = DTAR_decode_operation(opstr);

    const char* in_name = op->operand;
    int in_fd = mfu_open(in_name, O_RDONLY);

    const char* out_name = DTAR_writer.name;
    int out_fd = DTAR_writer.fd;

    uint64_t chunk_size = DTAR_opts->chunk_size;

    uint64_t in_offset  = chunk_size * op->chunk_index;
    uint64_t out_offset = op->offset + in_offset;

    mfu_lseek(in_name, in_fd, in_offset, SEEK_SET);
    mfu_lseek(out_name, out_fd, out_offset, SEEK_SET);

    ssize_t total_bytes_written = 0;
    while (total_bytes_written < chunk_size) {
        ssize_t num_of_bytes_read = mfu_read(in_name, in_fd, DTAR_iobuf, chunk_size);
        if (! num_of_bytes_read) {
            break;
        }
        ssize_t num_of_bytes_written = mfu_write(out_name, out_fd, DTAR_iobuf, num_of_bytes_read);
        total_bytes_written += num_of_bytes_written;
    }

    /* add bytes written into our reduce counter */
    reduce_bytes += total_bytes_written;

    uint64_t num_chunks = op->file_size / chunk_size;
    uint64_t rem = op->file_size - chunk_size * num_chunks;
    uint64_t last_chunk = (rem) ? num_chunks : num_chunks - 1;

    /* handle last chunk */
    if (op->chunk_index == last_chunk) {
        int padding = 512 - (int) (op->file_size % 512);
        if (padding > 0 && padding != 512) {
            char buff[512] = {0};
            mfu_write(out_name, out_fd, buff, padding);
        }
    }

    mfu_close(in_name, in_fd);
    mfu_free(&op);
}

void mfu_param_path_check_archive(
    int numparams,
    mfu_param_path* srcparams,
    mfu_param_path destparam,
    mfu_archive_opts_t* opts,
    int* valid)
{
    /* TODO: need to parallize this, rather than have every rank do the test */

    /* assume paths are valid */
    *valid = 1;

    /* count number of source paths that we can read */
    int i;
    int num_readable = 0;
    for (i = 0; i < numparams; i++) {
        char* path = srcparams[i].path;
        if (mfu_access(path, R_OK) == 0) {
            /* found one that we can read */
            num_readable++;
        } else {
            /* not readable */
            char* orig = srcparams[i].orig;
            MFU_LOG(MFU_LOG_ERR, "Could not read '%s' errno=%d %s",
                    orig, errno, strerror(errno));
        }
    }

    /* verify we have at least one valid source */
    if (num_readable < 1) {
        MFU_LOG(MFU_LOG_ERR, "At least one valid source must be specified");
        *valid = 0;
        goto bcast;
    }

    /* copy destination to user opts structure */
    opts->dest_path = MFU_STRDUP(destparam.path);

    /* check destination */
    if (destparam.path_stat_valid) {
        if (mfu_rank == 0) {
            MFU_LOG(MFU_LOG_WARN, "Destination target exists, we will overwrite");
        }
    } else {
        /* compute path to parent of destination archive */
        mfu_path* parent = mfu_path_from_str(destparam.path);
        mfu_path_dirname(parent);
        char* parent_str = mfu_path_strdup(parent);
        mfu_path_delete(&parent);

        /* check if parent is writable */
        if (mfu_access(parent_str, W_OK) < 0) {
            MFU_LOG(MFU_LOG_ERR, "Destination parent directory is not wriable: '%s' ",
                parent_str
            );
            *valid = 0;
            mfu_free(&parent_str);
            goto bcast;
        }

        mfu_free(&parent_str);
    }

    /* at this point, we know
     * (1) destination doesn't exist
     * (2) parent directory is writable
     */

bcast:
    MPI_Bcast(valid, 1, MPI_INT, 0, MPI_COMM_WORLD);

    if (! *valid) {
        if (mfu_rank == 0) {
            MFU_LOG(MFU_LOG_ERR, "Exiting run");
        }
        MPI_Barrier(MPI_COMM_WORLD);
        DTAR_exit(EXIT_FAILURE);
    }
}

static int write_entry_index(
    const char* file,
    uint64_t count,
    uint64_t* offsets)
{
    /* compute file name of index file */
    size_t namelen = strlen(file) + strlen(".idx") + 1;
    char* name = (char*) MFU_MALLOC(namelen);
    snprintf(name, namelen, "%s.idx", file);

    if (mfu_debug_level >= MFU_LOG_VERBOSE && mfu_rank == 0) {
        MFU_LOG(MFU_LOG_INFO, "Writing index to %s", name);
    }

    /* compute offset into index file for our entries */
    uint64_t offset;
    MPI_Scan(&count, &offset, 1, MPI_UINT64_T, MPI_SUM, MPI_COMM_WORLD);
    offset -= count;

    /* have rank 0 create and truncate the index file,
     * all others just open */
    int fd = -1;
    if (mfu_rank == 0) {
        mfu_unlink(name);
        fd = mfu_open(name, O_WRONLY | O_CREAT | O_TRUNC, 0660);
    }
    MPI_Barrier(MPI_COMM_WORLD);
    if (mfu_rank != 0) {
        fd = mfu_open(name, O_WRONLY, 0660);
    }

    /* compute size of memory buffer holding offsets */
    size_t bufsize = count * sizeof(uint64_t);

    /* pack offset values in network order */
    uint64_t i;
    uint64_t* packed = (uint64_t*) MFU_MALLOC(bufsize);
    char* ptr = (char*) packed;
    for (i = 0; i < count; i++) {
        mfu_pack_uint64(&ptr, offsets[i]);
    }

    /* each process writes offsets for its elements to the index */
    bool success = false;
    if (fd >= 0) {
        success = true;
        off_t off = offset * sizeof(uint64_t);
        ssize_t nwritten = mfu_pwrite(name, fd, packed, bufsize, off);
        if (nwritten != (ssize_t) bufsize) {
            success = false;
        }
        mfu_close(name, fd);
    }

    /* determine whether everyone succeeded */
    success = mfu_alltrue(success, MPI_COMM_WORLD);

    /* free buffer allocaed to hold packed offsets */
    mfu_free(&packed);

    /* free name of index file */
    mfu_free(&name);

    return success;
}

static int read_entry_index(
    const char* filename,
    uint64_t* out_count,
    uint64_t** out_offsets)
{
    /* assume we'll succeed */
    int rc = MFU_SUCCESS;

    /* assume we have the index file */
    int have_index = 1;

    /* compute file name of index file */
    size_t namelen = strlen(filename) + strlen(".idx") + 1;
    char* name = (char*) MFU_MALLOC(namelen);
    snprintf(name, namelen, "%s.idx", filename);

    /* compute number of entries based on file size */
    uint64_t count = 0;
    if (mfu_rank == 0) {
        struct stat st;
        int stat_rc = mfu_stat(name, &st);
        if (stat_rc == 0) {
            /* index stores one offset as uint64_t for each entry */
            count = st.st_size / sizeof(uint64_t);
        } else {
            /* failed to stat the index file */
            have_index = 0;
        }
    }

    /* broadcast number of entries to all ranks */
    MPI_Bcast(&count, 1, MPI_UINT64_T, 0, MPI_COMM_WORLD);

    /* read entry offsets from file */
    size_t bufsize = count * sizeof(uint64_t);
    uint64_t* offsets = (uint64_t*) MFU_MALLOC(bufsize);
    if (mfu_rank == 0 && have_index) {
        int fd = mfu_open(name, O_RDONLY);
        if (fd >= 0) {
            ssize_t nread = mfu_read(name, fd, offsets, bufsize);
            if (nread != bufsize) {
                /* have index file, but failed to read it */
                have_index = 0;
            }
            mfu_close(name, fd);
        } else {
            /* failed to open index file */
            have_index = 0;
        }
    }

    /* broadcast whether rank 0 could stat index file */
    MPI_Bcast(&have_index, 1, MPI_UINT64_T, 0, MPI_COMM_WORLD);

    /* bail out if we done have an index file */
    if (! have_index) {
        /* no index file, free memory and return failure */
        mfu_free(&offsets);
        mfu_free(&name);
        return MFU_FAILURE;
    }

    /* indicate to user what phase we're in */
    if (mfu_rank == 0) {
        MFU_LOG(MFU_LOG_INFO, "Read index %s", name);
    }

    /* convert offsets into host order */
    uint64_t i;
    uint64_t* packed = (uint64_t*) MFU_MALLOC(bufsize);
    const char* ptr = (const char*) offsets;
    for (i = 0; i < count; i++) {
        mfu_unpack_uint64(&ptr, &packed[i]);
    }

    /* free offsets we read from file */
    mfu_free(&offsets);

    /* broadcast offsets to all ranks */
    MPI_Bcast(packed, count, MPI_UINT64_T, 0, MPI_COMM_WORLD);

    /* free name of index file */
    mfu_free(&name);

    /* return count and list of offsets */
    *out_count   = count;
    *out_offsets = packed;

    return rc; 
}

/* set lustre stripe parameters on a file */
static void mfu_set_stripes(
    const char* file,    /* path of file to be striped */
    const char* cwd,     /* current working dir to prepend to file if not absolute */
    size_t stripe_bytes, /* width of a single stripe in bytes */
    int stripe_count)    /* number of stripes, -1 for all */
{
    /* get our rank */
    int rank;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    /* if file is on lustre, set striping parameters */
    if (rank == 0) {
        /* get absoluate path to file */
        mfu_path* dirpath = mfu_path_from_str(file);
        if (! mfu_path_is_absolute(dirpath)) {
            mfu_path_prepend_str(dirpath, cwd);
        }
        mfu_path_reduce(dirpath);

        /* get full path of item */
        const char* name = mfu_path_strdup(dirpath);

        /* get parent directory of item */
        mfu_path_dirname(dirpath);
        const char* dir = mfu_path_strdup(dirpath);

        /* if path is in lustre, configure the stripe parameters */
        if (mfu_is_lustre(dir)) {
            /* delete file incase it already exists */
            mfu_unlink(name);

            /* set striping parameters */
            mfu_stripe_set(name, stripe_bytes, stripe_count);
        }

        mfu_free(&name);
        mfu_free(&dir);
        mfu_path_delete(&dirpath);
    }

    MPI_Barrier(MPI_COMM_WORLD);

    return;
}

static int mfu_flist_archive_create_libcircle(
    mfu_flist inflist,
    const char* filename,
    int numpaths,
    const mfu_param_path* paths,
    const mfu_param_path* cwdpath,
    mfu_archive_opts_t* opts)
{
    int rc = MFU_SUCCESS;

    /* print note about what we're doing and the amount of files/data to be moved */
    if (mfu_debug_level >= MFU_LOG_VERBOSE && mfu_rank == 0) {
        MFU_LOG(MFU_LOG_INFO, "Writing archive to %s", filename);
    }

    /* print summary of item and byte count of items to be archived */
    mfu_flist_print_summary(inflist);

    /* start overall timer */
    time_t time_started;
    time(&time_started);
    double wtime_started = MPI_Wtime();

    /* sort items alphabetically, so they are placed in the archive with parent directories
     * coming before their children */
    mfu_flist flist = mfu_flist_sort("name", inflist);

    /* copy handles to objects into globals for libcircle operations */
    DTAR_flist = flist;
    DTAR_opts  = opts;

    /* if archive file will be on lustre, set max striping since this should be big */
    mfu_set_stripes(filename, cwdpath->path, opts->chunk_size, -1);

    /* create the archive file */
    DTAR_writer.name  = filename;
    DTAR_writer.flags = O_WRONLY | O_CREAT | O_CLOEXEC | O_LARGEFILE;
    DTAR_writer.fd    = mfu_open(filename, DTAR_writer.flags, 0664);

    /* Allocate a buffer to encode tar headers.
     * The entire header must fit in this buffer.
     * Typical entries will have no problems, but we may exhaust
     * space for entries that have very long ACLs or XATTRs. */
    DTAR_writer.bufsize = 128 * 1024 * 1024;
    DTAR_writer.buf = MFU_MALLOC(DTAR_writer.bufsize);

    /* get number of items in our portion of the list */
    uint64_t listsize = mfu_flist_size(flist);

    /* allocate memory for file sizes and offsets */
    uint64_t* fsizes  = (uint64_t*) MFU_MALLOC(listsize * sizeof(uint64_t));
    DTAR_offsets      = (uint64_t*) MFU_MALLOC(listsize * sizeof(uint64_t));
    DTAR_header_sizes = (uint64_t*) MFU_MALLOC(listsize * sizeof(uint64_t));

    /* allocate buffer to read/write data */
    DTAR_iobuf = MFU_MALLOC(opts->chunk_size);

    /* compute local offsets for each item and total
     * bytes we're contributing to the archive */
    uint64_t idx;
    uint64_t offset = 0;
    uint64_t data_bytes = 0;
    for (idx = 0; idx < listsize; idx++) {
        /* assume the item takes no space */
        DTAR_header_sizes[idx] = 0;
        fsizes[idx] = 0;

        /* identify item type to compute its size in the archive */
        mfu_filetype type = mfu_flist_file_get_type(flist, idx);
        if (type == MFU_TYPE_DIR || type == MFU_TYPE_LINK) {
            /* directories and symlinks only need the header */
            uint64_t header_size;
            encode_header(flist, idx, cwdpath,
                DTAR_writer.buf, DTAR_writer.bufsize, &header_size);
            DTAR_header_sizes[idx] = header_size;
            fsizes[idx] = header_size;
        } else if (type == MFU_TYPE_FILE) {
            /* regular file requires a header, plus file content,
             * and things are packed into blocks of 512 bytes */
            uint64_t header_size;
            encode_header(flist, idx, cwdpath,
                DTAR_writer.buf, DTAR_writer.bufsize, &header_size);
            DTAR_header_sizes[idx] = header_size;

            /* get file size of this item */
            uint64_t fsize = mfu_flist_file_get_size(flist, idx);

            /* round file size up to nearest integer number of 512 bytes */
            uint64_t fsize_padded = fsize / 512;
            fsize_padded *= 512;
            if (fsize_padded < fsize) {
                fsize_padded += 512;
            }

            /* entry size is the haeder plus the file data with padding */
            uint64_t entry_size = header_size + fsize_padded;
            fsizes[idx] += entry_size;

            /* increment our total data bytes */
            data_bytes += fsize_padded;
        }

        /* increment our local offset for this item */
        DTAR_offsets[idx] = offset;
        offset += fsizes[idx];
    }

    /* store total item and data byte count */
    DTAR_total_items = mfu_flist_global_size(flist);
    MPI_Allreduce(&data_bytes, &DTAR_total_bytes, 1, MPI_UINT64_T, MPI_SUM, MPI_COMM_WORLD);

    /* compute total archive size */
    uint64_t archive_size = 0;
    MPI_Allreduce(&offset, &archive_size, 1, MPI_UINT64_T, MPI_SUM, MPI_COMM_WORLD);

    /* execute scan to figure our global base offset in the archive file */
    uint64_t global_offset = 0;
    MPI_Scan(&offset, &global_offset, 1, MPI_UINT64_T, MPI_SUM, MPI_COMM_WORLD);
    global_offset -= offset;

    /* update offsets for each of our file to their global offset */
    for (idx = 0; idx < listsize; idx++) {
        DTAR_offsets[idx] += global_offset;
    }

    /* record global offsets in index */
    write_entry_index(filename, listsize, DTAR_offsets);

    /* print message to user that we're starting */
    if (mfu_debug_level >= MFU_LOG_VERBOSE && mfu_rank == 0) {
        MFU_LOG(MFU_LOG_INFO, "Truncating archive");
    }

    /* truncate file to correct size to overwrite existing file
     * and to preallocate space on the file system */
    if (mfu_rank == 0) {
        /* truncate to 0 to delete any existing file contents */
        mfu_ftruncate(DTAR_writer.fd, 0);

        /* truncate to proper size and preallocate space,
         * archive size represents the space to hold all entries,
         * then add on final two 512-blocks that mark the end of the archive */
        off_t final_size = archive_size + 2 * 512;
        mfu_ftruncate(DTAR_writer.fd, final_size);
        posix_fallocate(DTAR_writer.fd, 0, final_size);
    }
    MPI_Barrier(MPI_COMM_WORLD);

    /* print message to user that we're starting */
    if (mfu_debug_level >= MFU_LOG_VERBOSE && mfu_rank == 0) {
        MFU_LOG(MFU_LOG_INFO, "Writing entry headers");
    }

    /* write headers for our files */
    for (idx = 0; idx < listsize; idx++) {
        /* we currently only support regular files, directories, and symlinks */
        mfu_filetype type = mfu_flist_file_get_type(flist, idx);
        if (type == MFU_TYPE_FILE || type == MFU_TYPE_DIR || type == MFU_TYPE_LINK) {
            DTAR_write_header(flist, idx, cwdpath, DTAR_offsets[idx]);
        }
    }

    /* print message to user that we're starting */
    if (mfu_debug_level >= MFU_LOG_VERBOSE && mfu_rank == 0) {
        MFU_LOG(MFU_LOG_INFO, "Copying file data");
    }

    /* prepare libcircle */
    CIRCLE_init(0, NULL, CIRCLE_SPLIT_EQUAL | CIRCLE_CREATE_GLOBAL | CIRCLE_TERM_TREE);
    CIRCLE_loglevel loglevel = CIRCLE_LOG_WARN;
    CIRCLE_enable_logging(loglevel);

    /* register callbacks */
    CIRCLE_cb_create(&DTAR_enqueue_copy);
    CIRCLE_cb_process(&DTAR_perform_copy);

    /* prepare callbacks and initialize variables for reductions */
    reduce_start = MPI_Wtime();
    reduce_bytes = 0;
    CIRCLE_cb_reduce_init(&reduce_init);
    CIRCLE_cb_reduce_op(&reduce_exec);
    CIRCLE_cb_reduce_fini(&reduce_fini);

    /* set libcircle reduction period */
    int reduce_secs = 0;
    if (mfu_progress_timeout > 0) {
        reduce_secs = mfu_progress_timeout;
    }
    CIRCLE_set_reduce_period(reduce_secs);

    /* run the libcircle job to copy data into archive file */
    CIRCLE_begin();
    CIRCLE_finalize();

    /* rank 0 finalizes the archive by writing two 512-byte blocks of NUL
     * (according to tar file format) */
    if (mfu_rank == 0) {
        /* seek to end of archive */
        mfu_lseek(DTAR_writer.name, DTAR_writer.fd, archive_size, SEEK_SET);

        /* write two blocks of 512 bytes of 0 */
        char buf[1024] = {0};
        mfu_write(DTAR_writer.name, DTAR_writer.fd, buf, sizeof(buf));

        /* include final NULL blocks in our stats */
        archive_size += sizeof(buf);
    }

    /* close archive file */
    mfu_close(DTAR_writer.name, DTAR_writer.fd);

    /* wait for all ranks to finish */
    MPI_Barrier(MPI_COMM_WORLD);

    /* free sorted list */
    mfu_free(&flist);

    /* stop overall time */
    time_t time_ended;
    time(&time_ended);
    double wtime_ended = MPI_Wtime();

    /* print stats */
    double secs = wtime_ended - wtime_started;
    if (mfu_rank == 0) {
        char starttime_str[256];
        struct tm* localstart = localtime(&time_started);
        strftime(starttime_str, 256, "%b-%d-%Y, %H:%M:%S", localstart);

        char endtime_str[256];
        struct tm* localend = localtime(&time_ended);
        strftime(endtime_str, 256, "%b-%d-%Y, %H:%M:%S", localend);

        /* convert size to units */
        double size_tmp;
        const char* size_units;
        mfu_format_bytes(archive_size, &size_tmp, &size_units);

        /* convert bandwidth to unit */
        double agg_rate_tmp;
        double agg_rate = (double)archive_size / secs;
        const char* agg_rate_units;
        mfu_format_bw(agg_rate, &agg_rate_tmp, &agg_rate_units);

        MFU_LOG(MFU_LOG_INFO, "Started:   %s", starttime_str);
        MFU_LOG(MFU_LOG_INFO, "Completed: %s", endtime_str);
        MFU_LOG(MFU_LOG_INFO, "Seconds: %.3lf", secs);
        MFU_LOG(MFU_LOG_INFO, "Archive size: %.3lf %s", size_tmp, size_units);
        MFU_LOG(MFU_LOG_INFO, "Rate: %.3lf %s " \
                "(%.3" PRIu64 " bytes in %.3lf seconds)", \
                agg_rate_tmp, agg_rate_units, archive_size, secs);
    }

    /* clean up */
    mfu_free(&DTAR_writer.buf);
    mfu_free(&DTAR_iobuf);
    mfu_free(&fsizes);
    mfu_free(&DTAR_offsets);
    mfu_free(&DTAR_header_sizes);

    return rc;
}

int mfu_flist_archive_create(
    mfu_flist flist,
    const char* filename,
    int numpaths,
    const mfu_param_path* paths,
    const mfu_param_path* cwdpath,
    mfu_archive_opts_t* opts)
{
    int rc = mfu_flist_archive_create_libcircle(flist, filename, numpaths, paths, cwdpath, opts);
    return rc;
}

static int copy_data(struct archive* ar, struct archive* aw)
{
    int rc = MFU_SUCCESS;

    while (1) {
        /* extract a block of data from the archive */
        const void* buff;
        size_t size;
        off_t offset;
        int r = archive_read_data_block(ar, &buff, &size, &offset);
        if (r == ARCHIVE_EOF) {
            /* hit end of data for entry */
            break;
        }
        if (r != ARCHIVE_OK) {
            /* read error */
            MFU_LOG(MFU_LOG_ERR, "%s", archive_error_string(ar));
            rc = MFU_FAILURE;
            break;
        }

        /* write that block of data to the item on disk */
        r = archive_write_data_block(aw, buff, size, offset);
        if (r != ARCHIVE_OK) {
            /* write error */
            MFU_LOG(MFU_LOG_ERR, "%s", archive_error_string(ar));
            rc = MFU_FAILURE;
            break;
        }

        /* track number of bytes written so far */
        reduce_buf[REDUCE_BYTES] += (uint64_t) size;

        /* update number of items we have completed for progress messages */
        mfu_progress_update(reduce_buf, extract_prog);
    }

    return rc;
}

#if 0
static int count_entries(
    const char* filename,
    int flags,
    uint64_t* outcount)
{
    int r;

    /* assume we'll succeed */
    int rc = MFU_SUCCESS;

    /* indicate to user what phase we're in */
    if (mfu_rank == 0) {
        MFU_LOG(MFU_LOG_INFO, "Counting entries");
    }

    /* scan entire archive to count up number of entries */
    uint64_t count = 0;
    if (mfu_rank == 0) {
        /* initiate archive object for reading */
        struct archive* a = archive_read_new();

        /* we want all the format supports */
        archive_read_support_filter_bzip2(a);
        archive_read_support_filter_gzip(a);
        archive_read_support_filter_compress(a);
        archive_read_support_format_tar(a);

        /* read from stdin if not given a file? */
        if (filename != NULL && strcmp(filename, "-") == 0) {
            filename = NULL;
        }
    
        /* skipping through headers, so use a smaller blocksize */
        r = archive_read_open_filename(a, filename, 10240);
        if (r != ARCHIVE_OK) {
            MFU_LOG(MFU_LOG_ERR, "Failed to open archive %s",
                archive_error_string(a)
            );
            rc = MFU_FAILURE;
        }
    
        /* read entries one by one until we hit the EOF */
        while (rc == MFU_SUCCESS) {
            /* read header for the current entry */
            struct archive_entry* entry;
            r = archive_read_next_header(a, &entry);
            if (r == ARCHIVE_EOF) {
                /* found the end of the archive, we're done */
                break;
            }
            if (r != ARCHIVE_OK) {
                MFU_LOG(MFU_LOG_ERR, "Failed to read entry %s",
                    archive_error_string(a)
                );
                rc = MFU_FAILURE;
                break;
            }

            /* increment our count and move on to next entry */
            count++;
        }

        archive_read_close(a);
        archive_read_free(a);
    }
   
    /* get count of items from rank 0 */
    MPI_Bcast(&count, 1, MPI_UINT64_T, 0, MPI_COMM_WORLD);
    *outcount = count;

    /* broadcast whether rank 0 actually read archive successfully */
    MPI_Bcast(&rc, 1, MPI_INT, 0, MPI_COMM_WORLD);

    return rc; 
}
#endif

/* given a path to an archive, scan archive to determine number
 * of entries and the byte offset to each one */
static int index_entries(
    const char* filename,   /* name of archive to scan */
    uint64_t* out_count,    /* number of entries found in archive (set if successful) */
    uint64_t** out_offsets) /* list of byte offsets for each entry (if successful), caller must free */
{
    int r;

    /* assume we'll succeed */
    int rc = MFU_SUCCESS;

    /* indicate to user what phase we're in */
    if (mfu_rank == 0) {
        MFU_LOG(MFU_LOG_INFO, "Indexing archive");
    }

    /* have rank 0 scan archive to count up number of entries */
    uint64_t count = 0;
    uint64_t* offsets = NULL;
    if (mfu_rank == 0) {
        /* get file size so we can percent print progress as we go */
        uint64_t filesize = 0;
        struct stat st;
        int stat_rc = mfu_stat(filename, &st);
        if (stat_rc == 0) {
            /* stat succeeded, get the file size */
            filesize = st.st_size;
        } else {
            /* failed to stat the archive file,
             * we'll keep going, but progress messages will be disabled */
            MFU_LOG(MFU_LOG_ERR, "Failed to stat archive %s (errno=%d %s)",
                filename, errno, strerror(errno)
            );
        }

        /* initiate archive object for reading */
        struct archive* a = archive_read_new();

        /* cannot index an archive that is compressed, only a pure tar format */
//        archive_read_support_filter_bzip2(a);
//        archive_read_support_filter_gzip(a);
//        archive_read_support_filter_compress(a);
        archive_read_support_format_tar(a);

        /* read from stdin if not given a file? */
        if (filename != NULL && strcmp(filename, "-") == 0) {
            filename = NULL;
        }
    
        /* just scanning through headers, so we use a smaller blocksize */
        r = archive_read_open_filename(a, filename, 10240);
        if (r != ARCHIVE_OK) {
            /* failed to read archive, either file does not exist
             * or it may be a format we don't support */
            rc = MFU_FAILURE;
        }

#if 0
        /* TODO: scan for compression filters and bail out if we find any */
        // see archive_filter_count/code calls to iterate over filters
        int filter_count = archive_filter_count(a);
        int i;
        for (i = 0; i < filter_count; i++) {
            uint64_t filter_bytes = archive_filter_bytes(a, i);
            int filter_code = archive_filter_code(a, i);
            const char* filter_name = archive_filter_name(a, i);
            printf("bytes=%llu code=%d name=%s\n", filter_bytes, filter_code, filter_name);
        }
#endif

        /* start timer for progress messages */
        double start = MPI_Wtime();
        double last = start;
    
        /* read entries one by one until we hit the EOF */
        size_t maxcount = 1024;
        offsets = (uint64_t*) malloc(maxcount * sizeof(uint64_t));
        while (rc == MFU_SUCCESS) {
            /* increase our buffer capacity if needed */
            if (count >= maxcount) {
                /* ran out of slots, double capacity and allocate again */
                maxcount *= 2;
                offsets = realloc(offsets, maxcount * sizeof(uint64_t));
            }

            /* read header for the current entry */
            struct archive_entry* entry;
            r = archive_read_next_header(a, &entry);
            if (r == ARCHIVE_EOF) {
                /* found the end of the archive, we're done */
                break;
            }
            if (r != ARCHIVE_OK) {
                MFU_LOG(MFU_LOG_ERR, "Failed to read entry %s",
                    archive_error_string(a)
                );
                rc = MFU_FAILURE;
                break;
            }

            /* get offset of this header */
            uint64_t offset = (uint64_t) archive_read_header_position(a);
            offsets[count] = offset;

            /* increment our count and move on to next entry */
            count++;

            /* print progress message if needed */
            double now = MPI_Wtime();
            if (mfu_progress_timeout > 0 &&
                (now - last) > mfu_progress_timeout &&
                filesize > 0)
            {
                /* compute percent progress and estimated time remaining */
                double percent = (double)offset * 100.0 / (double)filesize;
                double secs = now - start;
                double secs_remaining = 0.0;
                if (percent > 0.0) {
                    secs_remaining = (double)(100.0 - percent) * secs / percent;
                }
                MFU_LOG(MFU_LOG_INFO, "Indexed %llu items in %.3lf secs (%.0f%%) %.0f secs left ...",
                    count, secs, percent, secs_remaining
                );
                last = now;
            }
        }

        /* print a final progress message if we may have printed any */
        double now = MPI_Wtime();
        double secs = now - start;
        if (rc == MFU_SUCCESS &&
            mfu_progress_timeout > 0 &&
            secs > mfu_progress_timeout)
        {
            MFU_LOG(MFU_LOG_INFO, "Indexed %llu items in %.3lf secs (100%%) done",
                count, secs
            );
        }

        /* close our read archive to clean up */
        archive_read_close(a);
        archive_read_free(a);
    }
   
    /* broadcast whether rank 0 actually read archive successfully */
    MPI_Bcast(&rc, 1, MPI_INT, 0, MPI_COMM_WORLD);

    /* bail out if rank 0 failed to index the archive */
    if (rc != MFU_SUCCESS) {
        mfu_free(&offsets);
        return rc;
    }

    /* get count of items from rank 0 */
    MPI_Bcast(&count, 1, MPI_UINT64_T, 0, MPI_COMM_WORLD);

    /* allocate memory to holding incoming offset values */
    if (mfu_rank != 0) {
        offsets = (uint64_t*) MFU_MALLOC(count * sizeof(uint64_t));
    }

    /* get offset values from rank 0 */
    MPI_Bcast(offsets, count, MPI_UINT64_T, 0, MPI_COMM_WORLD);

    /* return count and list of offsets */
    *out_count   = count;
    *out_offsets = offsets;

    return rc; 
}

/* given an entry data structure read from the archive,
 * create a corresponding item in the flist */
static void insert_entry_into_flist(
    struct archive_entry* entry, /* entry to be inserted */
    mfu_flist flist,             /* flist in which to insert item */
    const mfu_path* prefix)      /* prepend prefix to entry path to get absolute path for flist */
{
    uint64_t idx = mfu_flist_file_create(flist);

    const char* name = archive_entry_pathname(entry);

    /* name in the archive is relative,
     * but paths in flist are absolute (typically),
     * prepend given prefix and reduce resulting path */
    mfu_path* path = mfu_path_from_str(name);
    mfu_path_prepend(path, prefix);
    mfu_path_reduce(path);
    const char* name2 = mfu_path_strdup(path);
    mfu_flist_file_set_name(flist, idx, name2);
    mfu_free(&name2);
    mfu_path_delete(&path);

    /* get mode of entry, and deduce mfu type */
    mode_t mode = archive_entry_mode(entry);
    mfu_filetype type = mfu_flist_mode_to_filetype(mode);
    mfu_flist_file_set_type(flist, idx, type);

    mfu_flist_file_set_mode(flist, idx, mode);

    uint64_t uid = archive_entry_uid(entry);
    mfu_flist_file_set_uid(flist, idx, uid);

    uint64_t gid = archive_entry_gid(entry);
    mfu_flist_file_set_gid(flist, idx, gid);

    uint64_t atime = archive_entry_atime(entry);
    mfu_flist_file_set_atime(flist, idx, atime);

    uint64_t atime_nsec = archive_entry_atime_nsec(entry);
    mfu_flist_file_set_atime_nsec(flist, idx, atime_nsec);

    uint64_t mtime = archive_entry_mtime(entry);
    mfu_flist_file_set_mtime(flist, idx, mtime);

    uint64_t mtime_nsec = archive_entry_mtime_nsec(entry);
    mfu_flist_file_set_mtime_nsec(flist, idx, mtime_nsec);

    uint64_t ctime = archive_entry_ctime(entry);
    mfu_flist_file_set_ctime(flist, idx, ctime);

    uint64_t ctime_nsec = archive_entry_ctime_nsec(entry);
    mfu_flist_file_set_ctime_nsec(flist, idx, ctime_nsec);

    uint64_t size = archive_entry_size(entry);
    mfu_flist_file_set_size(flist, idx, size);
}

static int extract_flist_offsets(
    const char* filename,
    int flags,
    const mfu_param_path* cwdpath,
    uint64_t entries,
    uint64_t entry_start,
    uint64_t entry_count,
    uint64_t* offsets,
    mfu_flist flist)
{
    int r;

    /* assume we'll succeed */
    int rc = MFU_SUCCESS;

    /* indicate to user what phase we're in */
    if (mfu_rank == 0) {
        MFU_LOG(MFU_LOG_INFO, "Extracting metadata");
    }

    /* prepare list for metadata details */
    mfu_flist_set_detail(flist, 1);

    /* oppen archive file for readhing */
    int fd = mfu_open(filename, O_RDONLY);
    if (fd < 0) {
        MFU_LOG(MFU_LOG_ERR, "Failed to open archive: '%s' (errno=%d %s)",
            filename, errno, strerror(errno)
        );
        rc = MFU_FAILURE;
    }

    /* bail out with an error if anyone failed to open the archive */
    if (! mfu_alltrue(rc == MFU_SUCCESS, MPI_COMM_WORLD)) {
        return MFU_FAILURE;
    }

    /* get current working directory to prepend to
     * each entry to construct full path */
    mfu_path* cwd = mfu_path_from_str(cwdpath->path);

    /* iterate over each entry we're responsible for */
    uint64_t count = 0;
    while (count < entry_count) {
        /* compute offset and seek to this entry */
        uint64_t idx = entry_start + count;
        off_t offset = (off_t) offsets[idx];
        off_t pos = mfu_lseek(filename, fd, offset, SEEK_SET);
        if (pos == (off_t)-1) {
            MFU_LOG(MFU_LOG_ERR, "Failed to lseek to offset %llu in %s (errno=%d %s)",
                offset, filename, errno, strerror(errno)
            );
            rc = MFU_FAILURE;
            break;
        }

        /* initiate archive object for reading */
        struct archive* a = archive_read_new();

        /* when using an index, we can assume the archive is not compressed */
//        archive_read_support_filter_bzip2(a);
//        archive_read_support_filter_gzip(a);
//        archive_read_support_filter_compress(a);
        archive_read_support_format_tar(a);

        /* can use a small block size since we're just reading header info */
        r = archive_read_open_fd(a, fd, 10240);
        if (r != ARCHIVE_OK) {
            MFU_LOG(MFU_LOG_ERR, "Failed to open archive to extract entry %llu at offset %llu %s",
                idx, offset, archive_error_string(a)
            );
            rc = MFU_FAILURE;
            break;
        }

        /* read entry header from archive */
        struct archive_entry* entry;
        r = archive_read_next_header(a, &entry);
        if (r == ARCHIVE_EOF) {
            MFU_LOG(MFU_LOG_ERR, "Unexpected end of archive, read %llu of %llu entries",
                count, entry_count
            );
            rc = MFU_FAILURE;
            break;
        }
        if (r != ARCHIVE_OK) {
            MFU_LOG(MFU_LOG_ERR, "Failed to extract entry %llu at offset %llu %s",
                idx, offset, archive_error_string(a)
            );
            rc = MFU_FAILURE;
            break;
        }

        /* read the entry, create a corresponding flist entry for it */
        insert_entry_into_flist(entry, flist, cwd);

        /* close out the read archive, to be sure it doesn't have memory */
        r = archive_read_close(a);
        if (r != ARCHIVE_OK) {
            MFU_LOG(MFU_LOG_ERR, "Failed to close archive after extracting entry %llu at offset %llu %s",
                idx, offset, archive_error_string(a)
            );
            rc = MFU_FAILURE;
            break;
        }

        /* release read archive */
        r = archive_read_free(a);
        if (r != ARCHIVE_OK) {
            MFU_LOG(MFU_LOG_ERR, "Failed to free archive after extracting entry %llu at offset %llu %s",
                idx, offset, archive_error_string(a)
            );
            rc = MFU_FAILURE;
            break;
        }

        /* advance to next entry */
        count++;
    }

    mfu_flist_summarize(flist);

    mfu_path_delete(&cwd);

    mfu_close(filename, fd);

    /* check that all ranks succeeded */
    if (! mfu_alltrue(rc == MFU_SUCCESS, MPI_COMM_WORLD)) {
        rc = MFU_FAILURE;
    }

    return rc;
}

static void extract_flist(
    const char* filename,
    int flags,
    const mfu_param_path* cwdpath,
    uint64_t entries,
    uint64_t entry_start,
    uint64_t entry_count,
    mfu_flist flist)
{
    int r;

    /* prepare list for metadata details */
    mfu_flist_set_detail(flist, 1);

    /* indicate to user what phase we're in */
    if (mfu_rank == 0) {
        MFU_LOG(MFU_LOG_INFO, "Extracting metadata");
    }

    /* initiate archive object for reading */
    struct archive* a = archive_read_new();

    /* we want all the format supports */
    archive_read_support_filter_bzip2(a);
    archive_read_support_filter_gzip(a);
    archive_read_support_filter_compress(a);
    archive_read_support_format_tar(a);

    if (filename != NULL && strcmp(filename, "-") == 0) {
        filename = NULL;
    }

    /* blocksize set to 1024K */
    if ((r = archive_read_open_filename(a, filename, 10240))) {
        MFU_LOG(MFU_LOG_ERR, "%s", archive_error_string(a));
        exit(r);
    }

    /* get current working directory */
    mfu_path* cwd = mfu_path_from_str(cwdpath->path);

    int ranks;
    MPI_Comm_size(MPI_COMM_WORLD, &ranks);

    uint64_t count = 0;
    //while (entry_start + entry_count > count) {
    //while (count < entries) {
    while (1) {
        struct archive_entry* entry;
        r = archive_read_next_header(a, &entry);
        if (r == ARCHIVE_EOF) {
            break;
        }
        if (r != ARCHIVE_OK) {
            MFU_LOG(MFU_LOG_ERR, "%s", archive_error_string(a));
            exit(r);
        }

        //if (entry_start <= count) {
        if (count % ranks == mfu_rank) {
            insert_entry_into_flist(entry, flist, cwd);
        }

        count++;
    }

    mfu_flist_summarize(flist);

    mfu_path_delete(&cwd);

    archive_read_close(a);
    archive_read_free(a);

    return;
}

/* progress message to print while setting file metadata */
static void extract_progress_fn(const uint64_t* vals, int count, int complete, int ranks, double secs)
{
    /* compute average rate */
    double byte_rate = 0.0;
    double item_rate = 0.0;
    if (secs > 0) {
        byte_rate = (double)vals[REDUCE_BYTES] / secs;
        item_rate = (double)vals[REDUCE_ITEMS] / secs;
    }

    /* format number of bytes for printing */
    double bytes_val = 0.0;
    const char* bytes_units = NULL;
    mfu_format_bytes(vals[REDUCE_BYTES], &bytes_val, &bytes_units);

    /* format bandwidth for printing */
    double bw_val = 0.0;
    const char* bw_units = NULL;
    mfu_format_bw(byte_rate, &bw_val, &bw_units);

    /* compute percentage of bytes extracted */
    double percent = 0.0;
    if (DTAR_total_bytes > 0) {
        percent = (double)vals[REDUCE_BYTES] * 100.0 / (double)DTAR_total_bytes;
    }

    /* estimate seconds remaining */
    double secs_remaining = 0.0;
    if (byte_rate > 0.0) {
        secs_remaining = (double)(DTAR_total_bytes - vals[REDUCE_BYTES]) / byte_rate;
    }

    if (complete < ranks) {
        MFU_LOG(MFU_LOG_INFO,
            "Extracted %llu items and %.3lf %s (%.0f\%) in %.3lf secs (%.3lf items/sec, %.3lf %s) %.0f secs left ...",
            vals[REDUCE_ITEMS], bytes_val, bytes_units, percent, secs, item_rate, bw_val, bw_units, secs_remaining
        );
    } else {
        MFU_LOG(MFU_LOG_INFO,
            "Extracted %llu items and %.3lf %s (%.0f\%) in %.3lf secs (%.3lf items/sec, %.3lf %s) done",
            vals[REDUCE_ITEMS], bytes_val, bytes_units, percent, secs, item_rate, bw_val, bw_units
        );
    }
}

static int extract_files_offsets(
    const char* filename,
    int flags,
    uint64_t entries,
    uint64_t entry_start,
    uint64_t entry_count,
    uint64_t* offsets,
    mfu_flist flist,
    mfu_archive_opts_t* opts)
{
    int r;

    int rc = MFU_SUCCESS;

    /* indicate to user what phase we're in */
    if (mfu_rank == 0) {
        MFU_LOG(MFU_LOG_INFO, "Extracting items");
    }

    /* intitialize counters to track number of bytes and items extracted */
    reduce_buf[REDUCE_BYTES] = 0;
    reduce_buf[REDUCE_ITEMS] = 0;

    /* start progress messages while setting metadata */
    extract_prog = mfu_progress_start(mfu_progress_timeout, 2, MPI_COMM_WORLD, extract_progress_fn);

    /* open the archive file for reading */
    int fd = mfu_open(filename, O_RDONLY);
    if (fd < 0) {
        MFU_LOG(MFU_LOG_ERR, "Failed to open archive: '%s' errno=%d %s",
            filename, errno, strerror(errno)
        );
        rc = MFU_FAILURE;
    }

    /* initiate object for writing out items to disk */
    struct archive* ext = archive_write_disk_new();
    r = archive_write_disk_set_options(ext, flags);
    if (r != ARCHIVE_OK) {
        MFU_LOG(MFU_LOG_ERR, "Failed to set options on write object %s",
            archive_error_string(ext)
        );
        rc = MFU_FAILURE;
    }

    /* use system calls to lookup uname/gname (follows POSIX pax) */
    r = archive_write_disk_set_standard_lookup(ext);
    if (r != ARCHIVE_OK) {
        MFU_LOG(MFU_LOG_ERR, "Failed to set standard uname/gname lookup on write object %s",
            archive_error_string(ext)
        );
        rc = MFU_FAILURE;
    }

    /* iterate over and extract each item we're responsible for */
    uint64_t count = 0;
    while (count < entry_count && rc == MFU_SUCCESS) {
        /* seek to start of the entry in the archive file */
        uint64_t idx = entry_start + count;
        off_t offset = (off_t) offsets[idx];
        off_t pos = mfu_lseek(filename, fd, offset, SEEK_SET);
        if (pos == (off_t)-1) {
            MFU_LOG(MFU_LOG_ERR, "Failed to seek to offset %llu in open archive: '%s' errno=%d %s",
                offset, filename, errno, strerror(errno)
            );
            rc = MFU_FAILURE;
        }

        /* initiate archive object for reading */
        struct archive* a = archive_read_new();

        /* when using offsets, we assume there is no compression */
//        archive_read_support_filter_bzip2(a);
//        archive_read_support_filter_gzip(a);
//        archive_read_support_filter_compress(a);
        archive_read_support_format_tar(a);

        /* we can use a large blocksize for reading,
         * since we'll read headers and data in a contguous
         * region of the file */
        r = archive_read_open_fd(a, fd, opts->chunk_size);
        if (r != ARCHIVE_OK) {
            MFU_LOG(MFU_LOG_ERR, "opening archive to extract entry %llu at offset %llu %s",
                idx, offset, archive_error_string(a)
            );
            rc = MFU_FAILURE;
        }

        /* read the entry header for this item */
        struct archive_entry* entry;
        r = archive_read_next_header(a, &entry);
        if (r == ARCHIVE_EOF) {
            MFU_LOG(MFU_LOG_ERR, "unexpected end of archive, read %llu of %llu items",
                count, entry_count
            );
            rc = MFU_FAILURE;
            break;
        }
        if (r != ARCHIVE_OK) {
            MFU_LOG(MFU_LOG_ERR, "extracting entry %llu at offset %llu %s",
                idx, offset, archive_error_string(a)
            );
            rc = MFU_FAILURE;
        }

        /* got an entry, create corresponding item on disk */
        r = archive_write_header(ext, entry);
        if (r != ARCHIVE_OK) {
            MFU_LOG(MFU_LOG_ERR, "writing entry %llu at offset %llu %s",
                idx, offset, archive_error_string(ext)
            );
            rc = MFU_FAILURE;
        } else {
            /* extract file data (if item is a file) */
            int tmp_rc = copy_data(a, ext);
            if (tmp_rc != MFU_SUCCESS) {
                rc = tmp_rc;
            }
        }

        /* increment our count of items extracted */
        reduce_buf[REDUCE_ITEMS]++;

        /* update number of items we have completed for progress messages */
        mfu_progress_update(reduce_buf, extract_prog);

        /* close out the read archive object */
        r = archive_read_close(a);
        if (r != ARCHIVE_OK) {
            MFU_LOG(MFU_LOG_ERR, "Failed to close read archive %s",
                archive_error_string(a)
            );
            rc = MFU_FAILURE;
        }

        /* free memory allocated in read archive object */
        r = archive_read_free(a);
        if (r != ARCHIVE_OK) {
            MFU_LOG(MFU_LOG_ERR, "Failed to free read archive %s",
                archive_error_string(a)
            );
            rc = MFU_FAILURE;
        }

        /* advance to our next entry */
        count++;
    }

    /* finalize progress messages */
    mfu_progress_complete(reduce_buf, &extract_prog);

    /* Ensure all ranks have created all items before we close the write archive.
     * libarchive will update timestamps on directories when closing out,
     * so we want to ensure all child items exist at this point. */
    MPI_Barrier(MPI_COMM_WORLD);

    /* free off our write archive, this may update timestamps and permissions on items */
    r = archive_write_free(ext);
    if (r != ARCHIVE_OK) {
        MFU_LOG(MFU_LOG_ERR, "Failed to close archive for writing to disk %s",
            archive_error_string(ext)
        );
        rc = MFU_FAILURE;
    }

    /* TODO: if a directory already exists, libarchive does not currently update
     * its timestamps when closing the write archive,
     * update timestamps on directories */
    MPI_Barrier(MPI_COMM_WORLD);

    return rc;
}

static int extract_files(
    const char* filename,
    int flags,
    uint64_t entries,
    uint64_t entry_start,
    uint64_t entry_count,
    mfu_flist flist,
    mfu_archive_opts_t* opts)
{
    int r;

    int rc = MFU_SUCCESS;

    /* indicate to user what phase we're in */
    if (mfu_rank == 0) {
        MFU_LOG(MFU_LOG_INFO, "Extracting items");
    }

    /* intitialize counters to track number of bytes extracted */
    reduce_buf[REDUCE_BYTES] = 0;
    reduce_buf[REDUCE_ITEMS] = 0;

    /* start progress messages while setting metadata */
    extract_prog = mfu_progress_start(mfu_progress_timeout, 2, MPI_COMM_WORLD, extract_progress_fn);

    /* initiate archive object for reading */
    struct archive* a = archive_read_new();

    /* in the general case, we want potential compression
     * schemes in addition to tar format */
    archive_read_support_filter_bzip2(a);
    archive_read_support_filter_gzip(a);
    archive_read_support_filter_compress(a);
    archive_read_support_format_tar(a);

    /* initiate archive object for writing items out to disk */
    struct archive* ext = archive_write_disk_new();
    r = archive_write_disk_set_options(ext, flags);
    if (r != ARCHIVE_OK) {
        MFU_LOG(MFU_LOG_ERR, "Failed to set options on write object %s",
            archive_error_string(ext)
        );
        rc = MFU_FAILURE;
    }

    /* use system calls to lookup uname/gname (follows POSIX pax) */
    r = archive_write_disk_set_standard_lookup(ext);
    if (r != ARCHIVE_OK) {
        MFU_LOG(MFU_LOG_ERR, "Failed to set standard uname/gname lookup on write object %s",
            archive_error_string(ext)
        );
        rc = MFU_FAILURE;
    }

    /* read from stdin? */
    if (filename != NULL && strcmp(filename, "-") == 0) {
        filename = NULL;
    }

    //if ((r = archive_read_open_filename(a, filename, MFU_BLOCK_SIZE)))
    //r = archive_read_open_filename(a, filename, opts->chunk_size);
    r = archive_read_open_filename(a, filename, 1024 * 1024);
    if (r != ARCHIVE_OK) {
        MFU_LOG(MFU_LOG_ERR, "opening archive '%s' %s",
            filename, archive_error_string(a)
        );
        rc = MFU_FAILURE;
    }

    int ranks;
    MPI_Comm_size(MPI_COMM_WORLD, &ranks);

    /* iterate over all entry from the start of the file,
     * looking to find the range of items it is responsible for */
    uint64_t count = 0;
    //while (entry_start + entry_count > count) {
    //while (count < entries) {
    while (1) {
        /* read the next entry from the archive */
        struct archive_entry* entry;
        r = archive_read_next_header(a, &entry);
        if (r == ARCHIVE_EOF) {
            //MFU_LOG(MFU_LOG_ERR, "unexpected end of archive, read %llu of %llu items",
            //    count, entry_start + entry_count
            //);
            //rc = MFU_FAILURE;
            break;
        }
        if (r != ARCHIVE_OK) {
            MFU_LOG(MFU_LOG_ERR, "extracting entry %llu %s",
                count, archive_error_string(a)
            );
            rc = MFU_FAILURE;
        }

        /* write item out to disk if this is one of our assigned items */
        //if (entry_start <= count) {
        if (count % ranks == mfu_rank) {
            /* create item on disk */
            r = archive_write_header(ext, entry);
            if (r != ARCHIVE_OK) {
                MFU_LOG(MFU_LOG_ERR, "writing entry %llu %s",
                    count, archive_error_string(ext)
                );
                rc = MFU_FAILURE;
            } else {
                /* extract file data (if item is a file) */
                int tmp_rc = copy_data(a, ext);
                if (tmp_rc != MFU_SUCCESS) {
                    rc = tmp_rc;
                }
            }

            /* increment our count of items extracted */
            reduce_buf[REDUCE_ITEMS]++;

            /* update number of items we have completed for progress messages */
            mfu_progress_update(reduce_buf, extract_prog);
        }

        /* advance to next entry in the archive */
        count++;
    }

    /* finalize progress messages */
    mfu_progress_complete(reduce_buf, &extract_prog);

    /* Ensure all ranks have created all items before we close the write archive.
     * libarchive will update timestamps on directories when closing out,
     * so we want to ensure all child items exist at this point. */
    MPI_Barrier(MPI_COMM_WORLD);

    /* free off our write archive, this may update timestamps and permissions on items */
    r = archive_write_free(ext);
    if (r != ARCHIVE_OK) {
        MFU_LOG(MFU_LOG_ERR, "Failed to close archive for writing to disk %s",
            archive_error_string(ext)
        );
        rc = MFU_FAILURE;
    }

    /* close out the read archive object */
    r = archive_read_close(a);
    if (r != ARCHIVE_OK) {
        MFU_LOG(MFU_LOG_ERR, "Failed to close read archive %s",
            archive_error_string(a)
        );
        rc = MFU_FAILURE;
    }

    /* free memory allocated in read archive object */
    r = archive_read_free(a);
    if (r != ARCHIVE_OK) {
        MFU_LOG(MFU_LOG_ERR, "Failed to free read archive %s",
            archive_error_string(a)
        );
        rc = MFU_FAILURE;
    }

    /* TODO: if a directory already exists, libarchive does not currently update
     * its timestamps when closing the write archive,
     * update timestamps on directories */
    MPI_Barrier(MPI_COMM_WORLD);

    return rc;
}

/* compute total bytes in regular files in flist */
static uint64_t flist_sum_bytes(mfu_flist flist)
{
    /* sum up bytes in our portion of the list */
    uint64_t bytes = 0;
    if (mfu_flist_have_detail(flist)) {
        uint64_t idx = 0;
        uint64_t max = mfu_flist_size(flist);
        for (idx = 0; idx < max; idx++) {
            /* get size of regular files */
            mode_t mode = (mode_t) mfu_flist_file_get_mode(flist, idx);
            if (S_ISREG(mode)) {
                uint64_t size = mfu_flist_file_get_size(flist, idx);
                bytes += size;
            }
        }
    }

    /* get total bytes across all ranks */
    uint64_t total_bytes;
    MPI_Allreduce(&bytes, &total_bytes, 1, MPI_UINT64_T, MPI_SUM, MPI_COMM_WORLD);
    return total_bytes;
}

int mfu_flist_archive_extract(
    const char* filename,
    const mfu_param_path* cwdpath,
    mfu_archive_opts_t* opts)
{
    int r;

    int rc = MFU_SUCCESS;

    int ranks;
    MPI_Comm_size(MPI_COMM_WORLD, &ranks);

    /* configure flags for libarchive based on archive options */
    int flags = 0;
    flags |= ARCHIVE_EXTRACT_TIME;
    flags |= ARCHIVE_EXTRACT_OWNER;
    flags |= ARCHIVE_EXTRACT_PERM;

    /* turn on no overwrite so that directories we create are deleted and then replaced */
    //archive_opts.flags |= ARCHIVE_EXTRACT_NO_OVERWRITE;

    if (opts->preserve) {
        flags |= ARCHIVE_EXTRACT_XATTR;
        flags |= ARCHIVE_EXTRACT_ACL;
        flags |= ARCHIVE_EXTRACT_FFLAGS;
    }

    /* start overall timer */
    time_t time_started;
    time(&time_started);
    double wtime_started = MPI_Wtime();

    /* indicate to user what phase we're in */
    if (mfu_rank == 0) {
        MFU_LOG(MFU_LOG_INFO, "Extracting %s", filename);
    }

    /* get number of entries in archive */
    bool have_offsets = true;
    bool have_index = true;
    uint64_t entries = 0;
    uint64_t* offsets = NULL;
    int ret = read_entry_index(filename, &entries, &offsets);
    if (ret != MFU_SUCCESS) {
        /* don't have an index file */
        have_index = false;

        /* next best option is to scan the archive
         * and see if we can extract entry offsets */
        ret = index_entries(filename, &entries, &offsets);
        if (ret != MFU_SUCCESS) {
            /* failed to get entry offsets,
             * perhaps we have a compressed archive? */
            have_offsets = false;
        }

#if 0
        /* failed to read the index file, try the hard way
         * by scanning the archive from the start */
        have_index = false;
        ret = count_entries(filename, flags, &entries);
        if (ret != MFU_SUCCESS) {
            /* failed again, give up */
            return MFU_FAILURE;
        }
#endif
    }

    /* divide entries among ranks */
    uint64_t entries_per_rank = entries / ranks;
    uint64_t entries_remainder = entries - entries_per_rank * ranks;

    /* compute starting entry and number of entries based on our rank */
    uint64_t entry_start = 0;
    uint64_t entry_count = 0;
    if (mfu_rank < entries_remainder) {
        entry_count = entries_per_rank + 1;
        entry_start = mfu_rank * entry_count;
    } else {
        entry_count = entries_per_rank;
        entry_start = entries_remainder * (entry_count + 1) + (mfu_rank - entries_remainder) * entry_count;
    }

    /* extract metadata for items in archive and construct flist */
    mfu_flist flist = mfu_flist_new();
    if (have_offsets) {
        extract_flist_offsets(filename, flags, cwdpath, entries, entry_start, entry_count, offsets, flist);
    } else {
        extract_flist(filename, flags, cwdpath, entries, entry_start, entry_count, flist);
    }

    /* sum up bytes and items in list for tracking progress */
    DTAR_total_bytes = flist_sum_bytes(flist);
    DTAR_total_items = mfu_flist_global_size(flist);

    /* print summary of what's in archive before extracting items */
    mfu_flist_print_summary(flist);

    /* Create all directories in advance to avoid races between a process trying to create
     * a child item and another process responsible for the parent directory.
     * The libarchive code does not remove existing directories,
     * even in normal mode with overwrite. */
    /* indicate to user what phase we're in */
    if (mfu_rank == 0) {
        MFU_LOG(MFU_LOG_INFO, "Creating directories");
    }
    mfu_flist_mkdir(flist);

    /* TODO: We could precreate files and we'll need to if we allow more than one rank
     * to write to the same output file, however libarchive currently will delete these.
     * We'll need to figure out how to tell libarchive not to delete existing files. */
//    mfu_flist_mknod(flist);

    /* extract files from archive */
    if (have_offsets) {
        extract_files_offsets(filename, flags, entries, entry_start, entry_count, offsets, flist, opts);
    } else {
        extract_files(filename, flags, entries, entry_start, entry_count, flist, opts);
    }

    /* create list of just the directories */
    mfu_flist flist_dirs = mfu_flist_subset(flist);
    uint64_t idx;
    uint64_t size = mfu_flist_size(flist);
    for (idx = 0; idx < size; idx++) {
        /* if item is a directory, copy it to the directory list */
        mfu_filetype type = mfu_flist_file_get_type(flist, idx);
        if (type == MFU_TYPE_DIR) {
            mfu_flist_file_copy(flist, idx, flist_dirs);
        }
    }
    mfu_flist_summarize(flist_dirs);

    /* set timestamps on the directories, do this after writing all items
     * since creating items in a directory will have changed its timestamp */
    mfu_flist_metadata_apply(flist_dirs);

    /* free the list of directories */
    mfu_flist_free(&flist_dirs);

    /* if we constructed an offset list while unpacking the archive,
     * save it to an index file in case we need to unpack again */
    if (have_offsets && !have_index) {
        write_entry_index(filename, entry_count, &offsets[entry_start]);
    }

    /* we can now free our file list */
    mfu_flist_free(&flist);

    /* wait for all to finish */
    MPI_Barrier(MPI_COMM_WORLD);

    /* stop overall timer */
    time_t time_ended;
    time(&time_ended);
    double wtime_ended = MPI_Wtime();

    /* prep our values into buffer */
    int64_t values[2];
    values[0] = reduce_buf[REDUCE_ITEMS];
    values[1] = reduce_buf[REDUCE_BYTES];

    /* sum values across processes */
    int64_t sums[2];
    MPI_Allreduce(values, sums, 2, MPI_INT64_T, MPI_SUM, MPI_COMM_WORLD);

    /* extract results from allreduce */
    int64_t agg_items = sums[0];
    int64_t agg_bytes = sums[1];

    /* compute number of seconds */
    double secs = wtime_ended - wtime_started;

    /* compute rate of copy */
    double agg_bw = (double)agg_bytes / secs;
    if (secs > 0.0) {
        agg_bw = (double)agg_bytes / secs;
    }

    if(mfu_rank == 0) {
        /* format start time */
        char starttime_str[256];
        struct tm* localstart = localtime(&time_started);
        strftime(starttime_str, 256, "%b-%d-%Y, %H:%M:%S", localstart);

        /* format end time */
        char endtime_str[256];
        struct tm* localend = localtime(&time_ended);
        strftime(endtime_str, 256, "%b-%d-%Y, %H:%M:%S", localend);

        /* convert size to units */
        double agg_bytes_val;
        const char* agg_bytes_units;
        mfu_format_bytes((uint64_t)agg_bytes, &agg_bytes_val, &agg_bytes_units);

        /* convert bandwidth to units */
        double agg_bw_val;
        const char* agg_bw_units;
        mfu_format_bw(agg_bw, &agg_bw_val, &agg_bw_units);

        MFU_LOG(MFU_LOG_INFO, "Started:   %s", starttime_str);
        MFU_LOG(MFU_LOG_INFO, "Completed: %s", endtime_str);
        MFU_LOG(MFU_LOG_INFO, "Seconds: %.3lf", secs);
        MFU_LOG(MFU_LOG_INFO, "Items: %" PRId64, agg_items);
        MFU_LOG(MFU_LOG_INFO,
            "Data: %.3lf %s (%" PRId64 " bytes)",
            agg_bytes_val, agg_bytes_units, agg_bytes
        );
        MFU_LOG(MFU_LOG_INFO,
            "Rate: %.3lf %s (%.3" PRId64 " bytes in %.3lf seconds)",
            agg_bw_val, agg_bw_units, agg_bytes, secs
        );
    }

    return rc;
}

/* return a newly allocated archive_opts structure, set default values on its fields */
mfu_archive_opts_t* mfu_archive_opts_new(void)
{
    mfu_archive_opts_t* opts = (mfu_archive_opts_t*) MFU_MALLOC(sizeof(mfu_archive_opts_t));

    /* to record destination path that we'll be copying to */
    opts->dest_path = NULL;

    /* By default, don't bother to preserve all attributes. */
    opts->preserve = false;

    /* flags for libarchive */
    opts->flags = 0;

    /* size at which to slice up a file into units of work */
    opts->chunk_size = MFU_CHUNK_SIZE;

    /* buffer size for individual read/write operations */
    opts->block_size = MFU_BLOCK_SIZE;

    return opts;
}

void mfu_archive_opts_delete(mfu_archive_opts_t** popts)
{
  if (popts != NULL) {
    mfu_archive_opts_t* opts = *popts;

    /* free fields allocated on opts */
    if (opts != NULL) {
      mfu_free(&opts->dest_path);
    }

    mfu_free(popts);
  }
}