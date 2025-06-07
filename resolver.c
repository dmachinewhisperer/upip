//Greedy + Version Preference Resolver
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdbool.h>
#include <string.h>
#include "cJSON.h"

#include "upip.h"

//semver struct internal data structure
typedef struct {
    int major;
    int minor;
    int patch;
} semver_t;

//resolvedmap struct internal data structure
typedef struct {
    char **names;
    char **versions;
    int count;
} ResolvedMap;

static char *repo_resolve_version(const char *package, const char *constraint){

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "method", "GET_VER");
    cJSON_AddStringToObject(root, "package", package);
    cJSON_AddStringToObject(root, "constraint", constraint);

    char *response = upip_client_request_await_response(root, MEDIUM_TIMEOUT);
    cJSON_Delete(root);
    return response;
}

static cJSON *repo_get_metadata(const char *package, const char *version){
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "method", "GET_META");
    cJSON_AddStringToObject(root, "package", package);
    cJSON_AddStringToObject(root, "constraint", version);

    char *response = upip_client_request_await_response(root, MEDIUM_TIMEOUT);
    cJSON *response_json = cJSON_Parse(response);
    
    cJSON_Delete(root);
    if(response) free(response);
    
    return response_json;
}


static int parse_version(const char *version_str, semver_t *out) {
    if (!version_str || strcmp(version_str, "*") == 0) {
        out->major = out->minor = out->patch = 0;
        return 1; // valid wildcard
    }

    return sscanf(version_str, "%d.%d.%d", &out->major, &out->minor, &out->patch) == 3;
}

static int compare_versions(const semver_t *a, const semver_t *b) {
    if (a->major != b->major) return a->major - b->major;
    if (a->minor != b->minor) return a->minor - b->minor;
    return a->patch - b->patch;
}

static int semver_parser(const char *version_str, const char *constraint) {
    if (!constraint || !version_str) return 0;
    if (strcmp(constraint, "*") == 0) return 1;

    semver_t version, target;
    if (!parse_version(version_str, &version)) return 0;

    const char *op = constraint;
    const char *ver = NULL;

    if (strncmp(op, ">=", 2) == 0) {
        ver = op + 2;
        if (!parse_version(ver, &target)) return 0;
        return compare_versions(&version, &target) >= 0;
    } else if (strncmp(op, "<=", 2) == 0) {
        ver = op + 2;
        if (!parse_version(ver, &target)) return 0;
        return compare_versions(&version, &target) <= 0;
    } else if (strncmp(op, "==", 2) == 0) {
        ver = op + 2;
        if (strcmp(ver, "*") == 0) return 1;
        if (!parse_version(ver, &target)) return 0;
        return compare_versions(&version, &target) == 0;
    } else if (op[0] == '>' && isdigit(op[1])) {
        ver = op + 1;
        if (!parse_version(ver, &target)) return 0;
        return compare_versions(&version, &target) > 0;
    } else if (op[0] == '<' && isdigit(op[1])) {
        ver = op + 1;
        if (!parse_version(ver, &target)) return 0;
        return compare_versions(&version, &target) < 0;
    }

    return 0; // invalid constraint
}

static int satisfies_constraint(const char *version_str, const char *constraint_str) {
    if (!constraint_str || !version_str) return 0;

    char constraint_copy[64];
    strncpy(constraint_copy, constraint_str, sizeof(constraint_copy) - 1);
    constraint_copy[sizeof(constraint_copy) - 1] = '\0';

    char *token = strtok(constraint_copy, ",");
    while (token) {
        while (isspace((unsigned char)*token)) token++; // trim left

        if (!semver_parser(version_str, token)) {
            return 0; // one failure is enough
        }

        token = strtok(NULL, ",");
    }

    return 1; // all constraints passed
}


static int is_resolved(ResolvedMap *map, const char *name) {
    for (int i = 0; i < map->count; i++) {
        if (strcmp(map->names[i], name) == 0) return i;
    }
    return -1;
}

static void add_resolved(ResolvedMap *map, const char *name, const char *version) {
    map->names = realloc(map->names, sizeof(char *) * (map->count + 1));
    map->versions = realloc(map->versions, sizeof(char *) * (map->count + 1));
    map->names[map->count] = strdup(name);
    map->versions[map->count] = strdup(version);
    map->count++;
}

static void free_resolved(ResolvedMap *map) {
    for (int i = 0; i < map->count; i++) {
        free(map->names[i]);
        free(map->versions[i]);
    }
    free(map->names);
    free(map->versions);
}


// recursive resolver
static int resolve_recursive(const char *name, const char *constraint, ResolvedMap *resolved, cJSON *install_order) {
    // 🔍 First check if it's already installed
    if (is_installed(name)) {
        const char *installed_version = get_installed_version(name);
        if (!installed_version) {
            fprintf(stderr, "Installed package %s has no recorded version.\n", name);
            return -1;
        }

        if (satisfies_constraint(installed_version, constraint)) {
            // Already installed and satisfies constraint
            if (is_resolved(resolved, name) < 0) {
                add_resolved(resolved, name, installed_version);

                // Append to install order (to ensure deps come first)
                cJSON *pkg = cJSON_CreateObject();
                cJSON_AddStringToObject(pkg, "name", name);
                cJSON_AddStringToObject(pkg, "version", installed_version);
                cJSON_AddItemToArray(install_order, pkg);
            }
            return 0;
        } else {
            fprintf(stderr, "Installed version %s of %s does not satisfy constraint %s\n", installed_version, name, constraint);
            return -1;
        }
    }

    // Otherwise resolve from server
    char *version = repo_resolve_version(name, constraint);
    if (!version) {
        fprintf(stderr, "No version found for %s matching %s\n", name, constraint);
        return -1;
    }

    int existing = is_resolved(resolved, name);
    if (existing >= 0) {
        if (strcmp(resolved->versions[existing], version) != 0) {
            fprintf(stderr, "Conflict: %s already resolved to %s, can't use %s\n", name, resolved->versions[existing], version);
            free(version);
            return -1;
        }
        free(version);
        return 0;
    }

    cJSON *meta = repo_get_metadata(name, version);
    if (!meta) {
        fprintf(stderr, "Metadata fetch failed for %s@%s\n", name, version);
        free(version);
        return -1;
    }

    // Resolve dependencies first
    cJSON *deps = cJSON_GetObjectItem(meta, "dependencies");
    if (deps && cJSON_IsArray(deps)) {
        cJSON *dep = NULL;
        cJSON_ArrayForEach(dep, deps) { //this construct does not look c-like?
            const char *dep_name = cJSON_GetObjectItem(dep, "name")->valuestring;
            const char *dep_constraint = cJSON_GetObjectItem(dep, "version")->valuestring;
            if (resolve_recursive(dep_name, dep_constraint, resolved, install_order) != 0) {
                cJSON_Delete(meta);
                free(version);
                return -1;
            }
        }
    }

    // Mark this package as resolved
    add_resolved(resolved, name, version);

    // Append to install order
    cJSON *pkg = cJSON_CreateObject();
    cJSON_AddStringToObject(pkg, "name", name);
    cJSON_AddStringToObject(pkg, "version", version);
    cJSON_AddItemToArray(install_order, pkg);

    cJSON_Delete(meta);
    free(version);
    return 0;
}


cJSON *resolve(char *package, const char *constraint) {
    ResolvedMap resolved = {0};
    cJSON *install_order = cJSON_CreateArray();
    //* means install latest
    if (resolve_recursive(package, constraint, &resolved, install_order) != 0) {
        cJSON_Delete(install_order);
        free_resolved(&resolved);
        return NULL;
    }

    free_resolved(&resolved);
    return install_order;
}

