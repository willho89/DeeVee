#include "deevee_iso.h"

#include <ctype.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define DEEVEE_ISO_PRIMARY_VOLUME_DESCRIPTOR 1
#define DEEVEE_ISO_VOLUME_DESCRIPTOR_SECTOR 16
#define DEEVEE_ISO_STANDARD_ID_OFFSET 1
#define DEEVEE_ISO_ROOT_RECORD_OFFSET 156
#define DEEVEE_ISO_DIR_FLAG_DIRECTORY 0x02

static uint32_t read_le32(const uint8_t *data)
{
   return (uint32_t)data[0] |
      ((uint32_t)data[1] << 8) |
      ((uint32_t)data[2] << 16) |
      ((uint32_t)data[3] << 24);
}

static int parse_record(const uint8_t *record, size_t available,
      struct deevee_iso_entry *entry, const uint8_t **name,
      uint8_t *name_len)
{
   uint8_t length;

   if (!record || available < 34 || !entry || !name || !name_len)
      return 0;

   length = record[0];
   if (length < 34 || length > available)
      return 0;

   *name_len = record[32];
   if ((size_t)33 + *name_len > length)
      return 0;

   entry->lba = read_le32(record + 2);
   entry->size = read_le32(record + 10);
   entry->is_directory = (record[25] & DEEVEE_ISO_DIR_FLAG_DIRECTORY) != 0;
   *name = record + 33;
   return 1;
}

static int name_matches_component(const uint8_t *name, uint8_t name_len,
      const char *component)
{
   size_t i;
   size_t component_len;
   size_t effective_len = name_len;

   if (!name || !component)
      return 0;

   if (name_len == 1 && (name[0] == 0 || name[0] == 1))
      return 0;

   for (i = 0; i < name_len; i++)
      if (name[i] == ';')
      {
         effective_len = i;
         break;
      }

   component_len = strlen(component);
   if (component_len != effective_len)
      return 0;

   for (i = 0; i < effective_len; i++)
      if (toupper((unsigned char)component[i]) !=
            toupper((unsigned char)name[i]))
         return 0;

   return 1;
}

static enum deevee_iso_status read_primary_root(struct deevee_disc *disc,
      struct deevee_iso_entry *root)
{
   uint8_t sector[DEEVEE_DVD_SECTOR_SIZE];
   const uint8_t *name;
   uint8_t name_len;

   if (deevee_disc_read_sector(disc, DEEVEE_ISO_VOLUME_DESCRIPTOR_SECTOR,
            sector, sizeof(sector)) != DEEVEE_DISC_OK)
      return DEEVEE_ISO_ERROR_READ_FAILED;

   if (sector[0] != DEEVEE_ISO_PRIMARY_VOLUME_DESCRIPTOR ||
         memcmp(sector + DEEVEE_ISO_STANDARD_ID_OFFSET, "CD001", 5) != 0)
      return DEEVEE_ISO_ERROR_NOT_ISO9660;

   if (!parse_record(sector + DEEVEE_ISO_ROOT_RECORD_OFFSET,
            sizeof(sector) - DEEVEE_ISO_ROOT_RECORD_OFFSET,
            root, &name, &name_len) || !root->is_directory)
      return DEEVEE_ISO_ERROR_MALFORMED;

   (void)name;
   (void)name_len;
   return DEEVEE_ISO_OK;
}

static enum deevee_iso_status find_child(struct deevee_disc *disc,
      const struct deevee_iso_entry *directory, const char *component,
      struct deevee_iso_entry *entry)
{
   uint8_t sector[DEEVEE_DVD_SECTOR_SIZE];
   uint64_t offset = 0;

   if (!directory->is_directory)
      return DEEVEE_ISO_ERROR_NOT_FOUND;

   while (offset < directory->size)
   {
      uint64_t sector_index = directory->lba + offset / DEEVEE_DVD_SECTOR_SIZE;
      size_t sector_offset = (size_t)(offset % DEEVEE_DVD_SECTOR_SIZE);
      size_t available = DEEVEE_DVD_SECTOR_SIZE - sector_offset;
      const uint8_t *record;
      const uint8_t *name;
      uint8_t name_len;
      struct deevee_iso_entry candidate;

      if (deevee_disc_read_sector(disc, sector_index, sector,
               sizeof(sector)) != DEEVEE_DISC_OK)
         return DEEVEE_ISO_ERROR_READ_FAILED;

      record = sector + sector_offset;
      if (record[0] == 0)
      {
         offset += available;
         continue;
      }

      if (!parse_record(record, available, &candidate, &name, &name_len))
         return DEEVEE_ISO_ERROR_MALFORMED;

      if (name_matches_component(name, name_len, component))
      {
         *entry = candidate;
         return DEEVEE_ISO_OK;
      }

      offset += record[0];
   }

   return DEEVEE_ISO_ERROR_NOT_FOUND;
}

static const char *next_component(const char *path, char *component,
      size_t component_size)
{
   size_t len = 0;

   while (*path == '/')
      path++;

   while (path[len] && path[len] != '/')
   {
      if (len + 1 >= component_size)
         return NULL;
      component[len] = path[len];
      len++;
   }

   component[len] = '\0';
   while (path[len] == '/')
      len++;

   return path + len;
}

enum deevee_iso_status deevee_iso_find_path(struct deevee_disc *disc,
      const char *path, struct deevee_iso_entry *entry)
{
   enum deevee_iso_status status;
   struct deevee_iso_entry current;
   char component[128];

   if (!disc || !path || !path[0] || !entry)
      return DEEVEE_ISO_ERROR_INVALID_ARGUMENT;

   status = read_primary_root(disc, &current);
   if (status != DEEVEE_ISO_OK)
      return status;

   while (*path)
   {
      path = next_component(path, component, sizeof(component));
      if (!path)
         return DEEVEE_ISO_ERROR_MALFORMED;
      if (!component[0])
         break;

      status = find_child(disc, &current, component, &current);
      if (status != DEEVEE_ISO_OK)
         return status;
   }

   *entry = current;
   return DEEVEE_ISO_OK;
}

const char *deevee_iso_status_name(enum deevee_iso_status status)
{
   switch (status)
   {
      case DEEVEE_ISO_OK:
         return "ok";
      case DEEVEE_ISO_ERROR_INVALID_ARGUMENT:
         return "invalid_argument";
      case DEEVEE_ISO_ERROR_NOT_ISO9660:
         return "not_iso9660";
      case DEEVEE_ISO_ERROR_NOT_FOUND:
         return "not_found";
      case DEEVEE_ISO_ERROR_READ_FAILED:
         return "read_failed";
      case DEEVEE_ISO_ERROR_MALFORMED:
         return "malformed";
      default:
         return "unknown";
   }
}
