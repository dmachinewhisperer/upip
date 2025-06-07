#ifndef UPIP_H_
#define UPIP_H_

#include <stdbool.h>
#include "cJSON.h"

// File paths


// Timeout durations (in milliseconds)
#define SHORT_TIMEOUT                1000
#define MEDIUM_TIMEOUT               5000
#define LONG_TIMEOUT                10000

// Size limits (in bytes)
#define MAX_LINE_SIZE                5120
#define MAX_DLOAD_TRANSACTION_SIZE   5120


/**
 * platform abstraction layer
 */
//cJSON *load_file_to_json(const char *fname);
//void save_json_to_file(const char *fname, cJSON **json);
//int write_to_file(const char *filename, const char *data, size_t size);
char *upip_client_request_await_response(cJSON* message, int timeout_ms);

/**
 * public apis for accessing packages database
 */
bool is_installed(FATFS *fs,const char *name);
char *get_installed_version(FATFS *fs, const char *name); //returns malloc allocated string

/**
 * public api for the greedy solver
 */
cJSON *resolve(char *package, const char *constraint);

/**
 * public api for upip
 */
bool uninstall_package(FATFS *fs, const char *name);
bool install_package(FATFS *fs, const char *name, const char *constraints);
#endif // UPIP_H_
