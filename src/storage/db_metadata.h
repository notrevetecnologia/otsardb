#ifndef OTSARDB_DB_METADATA_H
#define OTSARDB_DB_METADATA_H

#include "object_store.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OTSARDB_DATABASE_ID_MAX 63

typedef struct otsardb_db_metadata {
    uint32_t format_version;
    uint32_t page_size;
    char database_id[OTSARDB_DATABASE_ID_MAX + 1];
} otsardb_db_metadata;

typedef enum otsardb_metadata_result {
    OTSARDB_METADATA_OK = 0,
    OTSARDB_METADATA_NOT_FOUND = 1,
    OTSARDB_METADATA_CONFLICT = 2,
    OTSARDB_METADATA_INVALID = 3,
    OTSARDB_METADATA_ERROR = 4
} otsardb_metadata_result;

int otsardb_db_metadata_encode_json(const otsardb_db_metadata *metadata, char **out_json, size_t *out_size);
int otsardb_db_metadata_decode_json(const char *json, size_t size, otsardb_db_metadata *out_metadata);

otsardb_metadata_result otsardb_db_metadata_create(otsardb_object_store *store,
                                                const char *database_root,
                                                const otsardb_db_metadata *metadata);

otsardb_metadata_result otsardb_db_metadata_read(otsardb_object_store *store,
                                              const char *database_root,
                                              otsardb_db_metadata *out_metadata);

#ifdef __cplusplus
}
#endif

#endif
