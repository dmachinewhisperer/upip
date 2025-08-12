#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"

#include "upip.h"
#include "upip_conf.h"
#include "lib/oofatfs/ff.h"
/*
#define CHUNK_SIZE                      8192  // 8 KB per chunk
#define MAX_FILE_PATH                   64
#define UPIP_PKGS_BASE_PATH             "/flash/upip_pkgs/"
#define REV_DEPS_TREE_FILE_PATH         "/flash/upip_pkgs/revdeptree.json"
#define INSTALLED_PKGS_DB_PATH          "/flash/upip_pkgs/pkgs.json"
#define FLASH_FS_ROOT_FS_PATH           "/flash"
*/

/**
 * return from a function
 */
#define RETURN_IF_ZERO(lvar, op) if (((lvar) = (op)) == 0) goto cleanup
#define RETURN_IF_NULL(lvar, op) if (((lvar) = (op)) == NULL) goto cleanup
#define RETURN_IF_FILE_ERROR(lvar, op) if (((lvar) = (op)) != FR_OK) goto cleanup

/**
 * break out one level out of an inner block
 */
#define BREAK_IF_ZERO(lvar, op) if (((lvar) = (op)) == 0) break
#define BREAK_IF_NULL(lvar, op) if (((lvar) = (op)) == NULL) break
#define BREAK_IF_FILE_ERROR(lvar, op) if (((lvar) = (op)) != FR_OK) break

/**
 * more specific macro for early return in executing Fatfs op sequence
 */
#define FTRY(op) RETURN_IF_FILE_ERROR(op, res)

/**
 * helper functions for doing fatfs fileio. 
 */
static FRESULT __f_load_file_to_json(FATFS *fs, const char *fpath, cJSON **json) {    
    FRESULT res;
    FIL file = {0};
    FILINFO fno = {0};
    UINT bytes_read;
    
    //FATFS * fs;
    //const char *pout;
    //__fs_fatfs_at_mount_point(FLASH_FS_ROOT_FS_PATH, &pout, &fs);

    FTRY(f_stat(fs, fpath, &fno));

    char *buf = NULL;
    buf = malloc(fno.fsize + 1);

    FTRY(f_open(fs, &file, fpath, FA_READ));
    FTRY(f_read(&file, buf, fno.fsize, &bytes_read));

    buf[bytes_read] = '\0'; 
    *json = cJSON_Parse(buf);

    res = FR_OK;
cleanup:
    f_close(&file); 
    if(buf) free(buf);
    return res;
}

static FRESULT __f_save_json_to_file(FATFS *fs, const char *fname, cJSON **json) {
    if (!json || !*json) return;

    FRESULT res;
    //FATFS * fs;
    //const char *pout;
    //__fs_fatfs_at_mount_point(FLASH_FS_ROOT_FS_PATH, &pout, &fs);
    // mount delegated

    char *string = cJSON_Print(*json);
    if (string) {
        FIL file = {0};
        UINT bw;

        FTRY(f_open(fs, &file, fname, FA_WRITE | FA_CREATE_ALWAYS));
        FTRY(f_write(&file, string, strlen(string), &bw));

        FTRY(f_sync(&file));

        free(string);
    }
cleanup:
    f_close(&file);
    return res;     
}

static FRESULT __f_rm_r(FATFS *fs, const char *path) {
    FF_DIR dir;
    FILINFO fno;
    FRESULT res;

    //FATFS * fs;
    //const char *pout;
    //__fs_fatfs_at_mount_point(FLASH_FS_ROOT_FS_PATH, &pout, &fs);    
    //mount delegated
    FTRY(f_opendir(fs, &dir, path));
    while (1) {
        FTRY(f_readdir(&dir, &fno));

        //skip . and ..
        if (strcmp(fno.fname, ".") == 0 || strcmp(fno.fname, "..") == 0) {
            continue;
        }
        char full_path[128];
        snprintf(full_path, sizeof(full_path), "%s/%s", path, fno.fname);

        if (fno.fattrib & AM_DIR) {
            FTRY(fatfs_delete_recursive(fs, full_path)); //recurse into subdirs
        } else {
            FTRY(f_unlink(fs, full_path));
        }
    }

    f_closedir(&dir);

    if (res == FR_OK) {
        FTRY(f_unlink(fs, path)); //delete the empty dir itself
    }
cleanup:
    return res;
}
/**
 * apis for managing installed_db state on disk. note that this function may be called from 
 * anywhere and must load the context it operates on . the wrapper functions is_installed and get_installed_version 
 * handle this case and load the context they operate on if executed standalone therefore they are the exposed apis
 */
 static bool _is_installed(FATFS* fs, const char *pkg_name, char **ver_out, cJSON *pkgs_installed) {
    bool pkgs_db_in_memory = true; 
    if (!pkgs_installed) {
        pkgs_db_in_memory = false; 
        __f_load_file_to_json(fs, INSTALLED_PKGS_DB_PATH, &pkgs_installed);
    }
    
    cJSON *item = cJSON_GetObjectItemCaseSensitive(pkgs_installed, pkg_name);
    bool result = cJSON_IsString(item) && item->valuestring;
    
    if (result && ver_out) {
        *ver_out = strdup(item->valuestring);  //caller must free!
    }

    if (!pkgs_db_in_memory) {
        cJSON_Delete(pkgs_installed);
    }
    
    return result;
}

bool is_installed(FATFS *fs char *pkg_name) {
    return _is_installed(fs, pkg_name, NULL, NULL);
}

char *get_installed_version(FATFS *fs, const char *pkg_name) {
    char *ver = NULL;
    if (_is_installed(fs, pkg_name, &ver, NULL)) {
        return ver;  //caller must free!
    }
    return NULL;
}

static void mark_installed(const char *name, const char *version, cJSON *pkgs_installed) {
    if (!name || !version || !pkgs_installed) {
        return;
    }
    cJSON_DeleteItemFromObjectCaseSensitive(pkgs_installed, name);
    cJSON_AddStringToObject(pkgs_installed, name, version);
}

static void mark_uninstalled(const char *name, cJSON *pkgs_installed) {
    if (!name || !pkgs_installed) {
        return;
    }
    cJSON_DeleteItemFromObjectCaseSensitive(pkgs_installed, name);
}

/**
 * helpers
 */
static void ensure_package_entry(cJSON *root, const char *name) {
    if (!cJSON_HasObjectItem(root, name)) {
        cJSON_AddItemToObject(root, name, cJSON_CreateArray());
    }
}

static void remove_item_from_array(cJSON *array, const char *value) {
    int index = 0;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, array) {
        if (strcmp(item->valuestring, value) == 0) {
            cJSON_DeleteItemFromArray(array, index);
            return;
        }
        index++;
    }
}


/**
 * apis for managing upips installed packages reverse dep tree
 */
static cJSON *get_reverse_dependencies(const char *name, cJSON *pkgs_rdt) {
    cJSON *deps = cJSON_GetObjectItemCaseSensitive(pkgs_rdt, name);
    if (!deps || !cJSON_IsArray(deps)) {
        return cJSON_CreateArray(); //return empty array if not found
    }

    cJSON *result = cJSON_Duplicate(deps, 1);
    return result;
}

static void add_reverse_dependency(const char *dependency, const char *dependent, cJSON *pkgs_rdt) {
    ensure_package_entry(pkgs_rdt, dependency);
    ensure_package_entry(pkgs_rdt, dependent);

    cJSON *array = cJSON_GetObjectItemCaseSensitive(pkgs_rdt, dependency);

    cJSON *item;
    cJSON_ArrayForEach(item, array) {
        if (strcmp(item->valuestring, dependent) == 0) {
            return; 
        }
    }

    cJSON_AddItemToArray(array, cJSON_CreateString(dependent));
}

static void remove_reverse_dependency(const char *dependency, const char *dependent, cJSON *pkgs_rdt) {
    cJSON *array = cJSON_GetObjectItemCaseSensitive(pkgs_rdt, dependency);
    if (array && cJSON_IsArray(array)) {
        remove_item_from_array(array, dependent);
    }
}

static int has_reverse_dependencies(const char *name, cJSON *pkgs_rdt) {
    cJSON *array = cJSON_GetObjectItemCaseSensitive(pkgs_rdt, name);
    int result = (array && cJSON_GetArraySize(array) > 0) ? 1 : 0;
    return result;
}

/**
 * procedure for downloading package from repository in a given filesystem fd
 * files stored on the server are assumed to be utf8 encoded .note that this function 
 * changes the state of the filesystem i.e writes new files and changes cwd to that of the newly installed package
 * 
 * TODO: preserve the state of fs before pkg installation
 */
static bool upip_download_pkg(FATFS *fs, const char *package, const char *version, FRESULT *fres_out) {
    bool ret = false;
    cJSON *request = NULL;
    cJSON *metadata = NULL;
    char *pkg_chunk = NULL; 
    cJSON *pkg_chunk_json = NULL;    

    RETURN_IF_NULL(metadata, (repo_get_metadata(package, version)));

    cJSON *files = cJSON_GetObjectItem(metadata, "files");
    int file_count;
    RETURN_IF_ZERO(file_count, (cJSON_GetArraySize(files)));

    request = cJSON_CreateObject();
    cJSON_AddStringToObject(request, "method", "getFileChunk");
    cJSON_AddStringToObject(request, "filename", "");
    cJSON_AddNumberToObject(request, "offset", 0);
    cJSON_AddNumberToObject(request, "length", CHUNK_SIZE);

    
    UINT bw;
    //FRESULT res;
    //FATFS * fs;
    //const char *pout;
    //RETURN_IF_FILE_ERROR((*fres_out), __fs_fatfs_at_mount_point(FLASH_FS_ROOT_FS_PATH, &pout, &fs)); //pout? possiblity of memory leakage!

    //change into the upip_pkgs dir, 
    //then into the package folder to reconstruct the package files
    RETURN_IF_FILE_ERROR((*fres_out), (f_chdir(fs, UPIP_PKGS_BASE_PATH)));
    RETURN_IF_FILE_ERROR((*fres_out), (f_mkdir(fs, package)));
    RETURN_IF_FILE_ERROR((*fres_out), (f_chdir(fs, package)));

    for (int i = 0; i < file_count; i++) {
        cJSON *file = cJSON_GetArrayItem(files, i);
        const char *filename = cJSON_GetObjectItem(file, "filename")->valuestring;
        int total_size = cJSON_GetObjectItem(file, "size")->valueint;

        FIL file;
        RETURN_IF_FILE_ERROR((*fres_out), (f_open(fs, &file, filename, FA_WRITE | FA_CREATE_ALWAYS)));
        int current_offset = 0;
        bool success = false;

        while (current_offset < total_size) {
            cJSON_ReplaceItemInObject(request, "filename", cJSON_CreateString(filename));
            cJSON_ReplaceItemInObject(request, "offset", cJSON_CreateNumber(current_offset));

            int remaining = total_size - current_offset;
            int chunk_size = (remaining < CHUNK_SIZE) ? remaining : CHUNK_SIZE;
            cJSON_ReplaceItemInObject(request, "length", cJSON_CreateNumber(chunk_size));

            BREAK_IF_NULL(pkg_chunk, (upip_client_request_await_response(request, MEDIUM_TIMEOUT)));
            BREAK_IF_NULL(pkg_chunk_json, (cJSON_Parse(pkg_chunk)));
            free(pkg_chunk);
            pkg_chunk = NULL; 

            if (!cJSON_GetObjectItem(pkg_chunk_json, "success")->valueint) {
                const char *error = cJSON_GetStringValue(cJSON_GetObjectItem(pkg_chunk_json, "message"));
                ESP_LOGE(TAG, "Server error: %s", error);
                break;
            }
            
            cJSON *result = cJSON_GetObjectItem(pkg_chunk_json, "result");
            const char *code_chunk = cJSON_GetObjectItem(result, "data")->valuestring

            int code_len = strlen(code_chunk);
            BREAK_IF_FILE_ERROR((*fres_out), (f_write(&file, code_chunk, code_len, &bw)));
            if (bw != code_len) {
                break;
            }

            cJSON_Delete(pkg_chunk_json);
            pkg_chunk_json = NULL; 

            current_offset += chunk_size;
            success = true; 
        }

        f_sync(&file);
        f_close(&file);

        if (!success) {
            //ESP_LOGE(TAG, "Download failed for file: %s", filename);
            //upip_client_stop();  // Optional: cancel ongoing transfer

            //back up and intermediate files
            RETURN_IF_FILE_ERROR((*fres_out), (f_chdir(fs, "..")));
            __f_rm_r(fs, package);
            ret = false;
            break; 
        }
       ret = true;  
    }
    
    //f_chdir(fs, FLASH_FS_ROOT_FS_PATH); //restore cwd to fs root

cleanup:
    if(pkg_chunk) free(pkg_chunk);
    if(request) cJSON_Delete(request);
    if(metadata) cJSON_Delete(metadata);
    if(pkg_chunk_json) cJSON_Delete(pkg_chunk_json);
    return ret;
}


bool install_package(FATFS *fs, const char *name, const char *constraint) {
    bool ret = false; 
    cJSON *plan = NULL;
    cJSON *pkgs_installed = NULL;
    cJSON *pkgs_revdeptree = NULL;

    __f_load_file_to_json(fs, INSTALLED_PKGS_DB_PATH, &pkgs_installed );
    __f_load_file_to_json(fs, REV_DEPS_TREE_FILE_PATH, &pkgs_revdeptree);
    if(!pkgs_installed || !pkgs_revdeptree) goto cleanup;

    RETURN_IF_NULL(plan, (resolve((char *)name, constraint)));

    int count = cJSON_GetArraySize(plan);
    for (int i = 0; i < count; i++) {
        cJSON *pkg = cJSON_GetArrayItem(plan, i);
        const char *pkg_name = cJSON_GetObjectItem(pkg, "name")->valuestring;
        const char *pkg_version = cJSON_GetObjectItem(pkg, "version")->valuestring;

        if (!_is_installed(fs, pkg_name, NULL, pkgs_installed)) {
            if (!upip_download_pkg(pkg_name, pkg_version)) {
                fprintf(stderr, "Failed to install %s@%s\n", pkg_name, pkg_version);
                goto cleanup;
            }

            mark_installed(pkg_name, pkg_version, pkgs_installed);

            //add reverse dep if not root package
            for (int j = 0; j < count; j++) {
                if (j != i) {
                    const char *dependent = cJSON_GetObjectItem(cJSON_GetArrayItem(plan, j), "name")->valuestring;
                    add_reverse_dependency(pkg_name, dependent, pkgs_revdeptree);
                }
            }
        }
    }
    
    __f_save_json_to_file(fs, INSTALLED_PKGS_DB_PATH, &pkgs_installed);
    __f_save_json_to_file(fs, REV_DEPS_TREE_FILE_PATH, pkgs_revdeptree);

    ret= true;

cleanup:
    if(plan) cJSON_Delete(plan);
    if(pkgs_installed) cJSON_Delete(pkgs_installed);
    if(pkgs_revdeptree) cJSON_Delete(pkgs_revdeptree);
    return ret; 
}

bool uninstall_package(FATFS *fs, const char *pkg_name) {
    bool ret = false;
    char *version = NULL;  
    cJSON *pkgs_installed = NULL;
    cJSON *pkgs_revdeptree = NULL;

    __f_load_file_to_json(fs, INSTALLED_PKGS_DB_PATH, &pkgs_installed );
    if (!_is_installed(fs, pkg_name, &version, pkgs_installed)){
        ret = true;
        goto cleanup;
    }

    __f_load_file_to_json(fs, REV_DEPS_TREE_FILE_PATH, &pkgs_revdeptree );
    if(!pkgs_installed || !pkgs_revdeptree) goto cleanup;

    if (has_reverse_dependencies(pkg_name, pkgs_revdeptree)) {
        fprintf(stderr, "Cannot uninstall %s: still required by other packages.\n", pkg_name);
        goto cleanup;
    }

    cJSON *meta = repo_get_metadata(pkg_name, version);
    if (!meta) goto cleanup;

    // Remove actual files (abstracted)
    //remove_package_files(pkg_name);
    __f_rm_r(fs, pkg_name);

    cJSON *deps = cJSON_GetObjectItem(meta, "dependencies");
    if (deps && cJSON_IsArray(deps)) {
        cJSON *dep = NULL;
        cJSON_ArrayForEach(dep, deps) {
            const char *dep_name = cJSON_GetObjectItem(dep, "name")->valuestring;
            remove_reverse_dependency(dep_name, pkg_name, pkgs_revdeptree);
        }
    }

    mark_uninstalled(pkg_name, pkgs_installed);

    //recursively remove orphaned dependencies
    if (deps && cJSON_IsArray(deps)) {
        cJSON *dep = NULL;
        cJSON_ArrayForEach(dep, deps) {
            const char *dep_name = cJSON_GetObjectItem(dep, "name")->valuestring;
            if (!has_reverse_dependencies(dep_name, pkgs_revdeptree)) {
                uninstall_package(dep_name);
            }
        }
    }
    __f_save_json_to_file(fs, INSTALLED_PKGS_DB_PATH, pkgs_installed);
    __f_save_json_to_file(fs, REV_DEPS_TREE_FILE_PATH, pkgs_revdeptree);
    ret = true; 

cleanup:   
    if(meta) cJSON_Delete(plan);
    if(pkgs_installed) cJSON_Delete(pkgs_installed);
    if(pkgs_revdeptree) cJSON_Delete(pkgs_revdeptree);
    return ret; 
}
