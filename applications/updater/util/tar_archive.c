#include "tar_archive.h"

#include <microtar.h>
#include <storage/storage.h>
#include <furi.h>

#define TAG "TarArch"
#define MAX_NAME_LEN 255
#define FILE_BLOCK_SIZE 512

#define FILE_OPEN_NTRIES 10
#define FILE_OPEN_RETRY_DELAY 25

typedef struct TarArchive {
    Storage* storage;
    mtar_t tar;
} TarArchive;

/* API WRAPPER */
static int mtar_storage_file_write(void* stream, const void* data, unsigned size) {
    uint16_t bytes_written = storage_file_write(stream, data, size);
    return (bytes_written == size) ? bytes_written : MTAR_EWRITEFAIL;
}

static int mtar_storage_file_read(void* stream, void* data, unsigned size) {
    uint16_t bytes_read = storage_file_read(stream, data, size);
    return (bytes_read == size) ? bytes_read : MTAR_EREADFAIL;
}

static int mtar_storage_file_seek(void* stream, unsigned offset) {
    bool res = storage_file_seek(stream, offset, true);
    return res ? MTAR_ESUCCESS : MTAR_ESEEKFAIL;
}

static int mtar_storage_file_close(void* stream) {
    storage_file_close(stream);
    return MTAR_ESUCCESS;
}

const struct mtar_ops filesystem_ops = {
    .read = mtar_storage_file_read,
    .write = mtar_storage_file_write,
    .seek = mtar_storage_file_seek,
    .close = mtar_storage_file_close,
};

TarArchive* tar_archive_alloc(Storage* storage) {
    furi_check(storage);
    TarArchive* archive = malloc(sizeof(TarArchive));
    archive->storage = storage;
    return archive;
}

bool tar_archive_open(TarArchive* archive, const char* path, TarOpenMode mode) {
    furi_assert(archive);
    FS_AccessMode access_mode;
    FS_OpenMode open_mode;
    int mtar_access = 0;

    switch(mode) {
    case TAR_OPEN_MODE_READ:
        mtar_access = MTAR_READ;
        access_mode = FSAM_READ;
        open_mode = FSOM_OPEN_EXISTING;
        break;
    case TAR_OPEN_MODE_WRITE:
        mtar_access = MTAR_WRITE;
        access_mode = FSAM_WRITE;
        open_mode = FSOM_CREATE_ALWAYS;
        break;
    default:
        return false;
    }

    File* stream = storage_file_alloc(archive->storage);
    if(!storage_file_open(stream, path, access_mode, open_mode)) {
        storage_file_free(stream);
        return false;
    }
    mtar_init(&archive->tar, mtar_access, &filesystem_ops, stream);

    return true;
}

void tar_archive_free(TarArchive* archive) {
    furi_assert(archive);
    mtar_close(&archive->tar);
}

bool tar_archive_dir_add_element(TarArchive* archive, const char* dirpath) {
    furi_assert(archive);
    return (mtar_write_dir_header(&archive->tar, dirpath) == MTAR_ESUCCESS);
}

bool tar_archive_finalize(TarArchive* archive) {
    furi_assert(archive);
    return (mtar_finalize(&archive->tar) == MTAR_ESUCCESS);
}

bool tar_archive_store_data(
    TarArchive* archive,
    const char* path,
    const uint8_t* data,
    const int32_t data_len) {
    furi_assert(archive);

    return (
        tar_archive_file_add_header(archive, path, data_len) &&
        tar_archive_file_add_data_block(archive, data, data_len) &&
        tar_archive_file_finalize(archive));
}

bool tar_archive_file_add_header(TarArchive* archive, const char* path, const int32_t data_len) {
    furi_assert(archive);

    return (mtar_write_file_header(&archive->tar, path, data_len) == MTAR_ESUCCESS);
}

bool tar_archive_file_add_data_block(
    TarArchive* archive,
    const uint8_t* data_block,
    const int32_t block_len) {
    furi_assert(archive);

    return (mtar_write_data(&archive->tar, data_block, block_len) == block_len);
}

bool tar_archive_file_finalize(TarArchive* archive) {
    furi_assert(archive);
    return (mtar_end_data(&archive->tar) == MTAR_ESUCCESS);
}

typedef struct {
    TarArchive* archive;
    const char* work_dir;
} TarArchiveDirectoryOpParams;

static int archive_extract_foreach_cb(mtar_t* tar, const mtar_header_t* header, void* param) {
    TarArchiveDirectoryOpParams* op_params = param;
    string_t fname;

    const int TAR_READ_BUF_SZ = 512;

    if(header->type == MTAR_TDIR) {
        string_init_printf(fname, "%s/%s", op_params->work_dir, header->name);

        bool create_res =
            storage_simply_mkdir(op_params->archive->storage, string_get_cstr(fname));
        string_clear(fname);
        return create_res ? 0 : -1;
    }

    if(header->type != MTAR_TREG) {
        FURI_LOG_W(TAG, "not extracting unsupported type \"%s\"", header->name);
        return 0;
    }

    string_init_printf(fname, "%s/%s", op_params->work_dir, header->name);
    FURI_LOG_I(TAG, "Extracting %d bytes to '%s'", header->size, header->name);
    File* out_file = storage_file_alloc(op_params->archive->storage);
    uint8_t* readbuf = malloc(TAR_READ_BUF_SZ);

    bool failed = false;
    uint8_t n_tries = FILE_OPEN_NTRIES;
    do {
        while(
            (n_tries-- > 0) &&
            !storage_file_open(out_file, string_get_cstr(fname), FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
            FURI_LOG_W(TAG, "Failed to open '%s', reties: %d", string_get_cstr(fname), n_tries);
            osDelay(FILE_OPEN_RETRY_DELAY);
            continue;
        }

        if(!storage_file_is_open(out_file)) {
            failed = true;
            break;
        }

        while(!mtar_eof_data(tar)) {
            int32_t readcnt = mtar_read_data(tar, readbuf, TAR_READ_BUF_SZ);
            if(!readcnt || !storage_file_write(out_file, readbuf, readcnt)) {
                failed = true;
                break;
            }
        }
    } while(false);

    storage_file_free(out_file);
    free(readbuf);
    string_clear(fname);
    return failed ? -1 : 0;
}

bool tar_archive_unpack_to(TarArchive* archive, const char* destination) {
    furi_assert(archive);
    TarArchiveDirectoryOpParams param = {
        .archive = archive,
        .work_dir = destination,
    };

    FURI_LOG_I(TAG, "Restoring '%s'", destination);

    return (mtar_foreach(&archive->tar, archive_extract_foreach_cb, &param) == MTAR_ESUCCESS);
};

bool tar_archive_add_file(
    TarArchive* archive,
    const char* fs_file_path,
    const char* archive_fname,
    const int32_t file_size) {
    furi_assert(archive);
    uint8_t* file_buffer = malloc(FILE_BLOCK_SIZE);
    bool success = false;
    File* src_file = storage_file_alloc(archive->storage);
    uint8_t n_tries = FILE_OPEN_NTRIES;
    do {
        while((n_tries-- > 0) &&
              !storage_file_open(src_file, fs_file_path, FSAM_READ, FSOM_OPEN_EXISTING)) {
            FURI_LOG_W(TAG, "Failed to open '%s', reties: %d", fs_file_path, n_tries);
            osDelay(FILE_OPEN_RETRY_DELAY);
            continue;
        }

        if(!storage_file_is_open(src_file) ||
           !tar_archive_file_add_header(archive, archive_fname, file_size)) {
            break;
        }

        uint16_t bytes_read = 0;
        while((bytes_read = storage_file_read(src_file, file_buffer, FILE_BLOCK_SIZE))) {
            success = tar_archive_file_add_data_block(archive, file_buffer, bytes_read);
            if(!success) {
                break;
            }
        }

        success = success && tar_archive_file_finalize(archive);
    } while(false);

    storage_file_free(src_file);
    free(file_buffer);
    return success;
}

bool tar_archive_add_dir(TarArchive* archive, const char* fs_full_path, const char* path_prefix) {
    furi_assert(archive);
    furi_check(path_prefix);
    File* directory = storage_file_alloc(archive->storage);
    FileInfo file_info;

    FURI_LOG_I(TAG, "Backing up '%s', '%s'", fs_full_path, path_prefix);
    char* name = malloc(MAX_NAME_LEN);
    bool success = false;

    do {
        if(!storage_dir_open(directory, fs_full_path)) {
            break;
        }

        while(true) {
            if(!storage_dir_read(directory, &file_info, name, MAX_NAME_LEN)) {
                success = true; // empty dir / no more files
                break;
            }

            string_t element_name, element_fs_abs_path;
            string_init_printf(element_fs_abs_path, "%s/%s", fs_full_path, name);
            if(strlen(path_prefix)) {
                string_init_printf(element_name, "%s/%s", path_prefix, name);
            } else {
                string_init_set(element_name, name);
            }

            if(file_info.flags & FSF_DIRECTORY) {
                success = tar_archive_dir_add_element(archive, string_get_cstr(element_name)) &&
                          tar_archive_add_dir(
                              archive,
                              string_get_cstr(element_fs_abs_path),
                              string_get_cstr(element_name));
            } else {
                success = tar_archive_add_file(
                    archive,
                    string_get_cstr(element_fs_abs_path),
                    string_get_cstr(element_name),
                    file_info.size);
            }
            string_clear(element_name);
            string_clear(element_fs_abs_path);

            if(!success) {
                break;
            }
        }
    } while(false);

    free(name);
    storage_file_free(directory);
    return success;
}