#ifndef METADATA_PARSER_INTERNAL_H
#define METADATA_PARSER_INTERNAL_H

#define METADATA_BLOB_MAX_BYTES (4U * 1024U * 1024U)
#define EMBEDDED_COVER_MAX_BYTES METADATA_BLOB_MAX_BYTES

/* One-way mode used only by the disposable artwork helper process. */
void metadata_parser_enable_artwork_only(void);

#endif /* METADATA_PARSER_INTERNAL_H */
