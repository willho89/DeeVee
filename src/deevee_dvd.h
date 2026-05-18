#ifndef DEEVEE_DVD_H
#define DEEVEE_DVD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "deevee_disc.h"
#include "deevee_iso.h"

#define DEEVEE_DVD_VMG_IDENTIFIER "DVDVIDEO-VMG"
#define DEEVEE_DVD_VTS_IDENTIFIER "DVDVIDEO-VTS"
#define DEEVEE_DVD_IFO_PROBE_BYTES 2048
#define DEEVEE_DVD_MAX_TITLES 99
#define DEEVEE_DVD_MAX_MENU_LANGUAGE_UNITS 16
#define DEEVEE_DVD_MAX_MENU_BUTTONS 36
#define DEEVEE_DVD_MAX_PROBED_COMMANDS 8

enum deevee_dvd_status
{
   DEEVEE_DVD_OK = 0,
   DEEVEE_DVD_ERROR_INVALID_ARGUMENT,
   DEEVEE_DVD_ERROR_IFO_NOT_FOUND,
   DEEVEE_DVD_ERROR_READ_FAILED,
   DEEVEE_DVD_ERROR_INVALID_VMG,
   DEEVEE_DVD_ERROR_TABLE_NOT_FOUND,
   DEEVEE_DVD_ERROR_MALFORMED
};

struct deevee_dvd_info
{
   struct deevee_iso_entry video_ts_ifo;
   char vmg_identifier[13];
   bool is_dvd_video;
   uint32_t vmg_last_sector;
   uint32_t vmgi_last_sector;
   uint16_t vmg_title_set_count;
   uint32_t vmgi_last_byte;
   uint32_t first_play_pgc;
   uint32_t vmgm_vobs;
   uint32_t tt_srpt;
   uint32_t vmgm_pgci_ut;
   uint32_t ptl_mait;
   uint32_t vts_atrt;
   uint32_t txtdt_mgi;
   uint32_t vmgm_c_adt;
   uint32_t vmgm_vobu_admap;
};

struct deevee_dvd_title
{
   uint8_t title_type;
   uint8_t angle_count;
   uint16_t chapter_count;
   uint16_t parental_management_mask;
   uint8_t vts_number;
   uint8_t vts_title_number;
   uint32_t vts_start_sector;
};

struct deevee_dvd_title_table
{
   uint16_t title_count;
   uint32_t last_byte;
   uint16_t parsed_title_count;
   struct deevee_dvd_title titles[DEEVEE_DVD_MAX_TITLES];
};

struct deevee_dvd_menu_language_unit
{
   char language[3];
   uint8_t language_extension;
   uint8_t menu_existence;
   uint32_t start_byte;
};

struct deevee_dvd_menu_language_table
{
   uint16_t language_count;
   uint32_t last_byte;
   uint16_t parsed_language_count;
   struct deevee_dvd_menu_language_unit
      languages[DEEVEE_DVD_MAX_MENU_LANGUAGE_UNITS];
};

struct deevee_dvd_menu_pgc_summary
{
   char language[3];
   uint32_t language_unit_start_byte;
   uint16_t pgc_count;
   uint32_t language_unit_last_byte;
   uint32_t pgc_category;
   uint32_t pgc_start_byte;
   uint8_t program_count;
   uint8_t cell_count;
   uint8_t playback_time[4];
   uint32_t prohibited_user_ops;
   uint16_t command_table_offset;
   uint16_t program_map_offset;
   uint16_t cell_playback_table_offset;
   uint16_t cell_position_table_offset;
};

struct deevee_dvd_menu_pgc_table_summary
{
   char language[3];
   uint16_t pre_command_count;
   uint16_t post_command_count;
   uint16_t cell_command_count;
   uint32_t command_table_last_byte;
   uint8_t first_program_entry_cell;
   uint32_t first_cell_category;
   uint8_t first_cell_playback_time[4];
   uint32_t first_cell_first_vobu_start_sector;
   uint32_t first_cell_first_ilvu_end_sector;
   uint32_t first_cell_last_vobu_start_sector;
   uint32_t first_cell_last_vobu_end_sector;
   uint16_t first_cell_vob_id;
   uint8_t first_cell_id;
};

struct deevee_dvd_menu_vob_span
{
   struct deevee_iso_entry vmgm_vob;
   uint32_t vmgm_vob_sector_count;
   uint32_t first_cell_start_sector;
   uint32_t first_cell_end_sector;
   uint64_t first_cell_start_lba;
   uint64_t first_cell_end_lba;
   uint16_t first_cell_vob_id;
   uint8_t first_cell_id;
};

struct deevee_dvd_vob_packet_probe
{
   uint32_t scanned_sectors;
   uint32_t pack_header_count;
   uint32_t system_header_count;
   uint32_t program_stream_map_count;
   uint32_t private_stream_1_count;
   uint32_t private_stream_2_count;
   uint32_t padding_stream_count;
   uint32_t video_pes_count;
   uint32_t audio_pes_count;
   uint32_t ac3_audio_count;
   uint32_t dts_audio_count;
   uint32_t lpcm_audio_count;
   uint32_t subpicture_count;
   uint32_t private_stream_1_unknown_count;
   uint32_t nav_pci_count;
   uint32_t nav_dsi_count;
   uint32_t private_stream_2_unknown_count;
   uint32_t other_pes_count;
   uint32_t nav_pack_count;
   uint8_t first_video_stream_id;
   uint8_t first_audio_stream_id;
   uint8_t first_private_stream_1_substream_id;
   uint8_t first_nav_substream_id;
   bool has_pack_header;
   bool has_video;
   bool has_audio;
   bool has_subpicture;
   bool has_nav;
};

struct deevee_dvd_video_probe
{
   uint32_t scanned_sectors;
   uint32_t video_pes_packets;
   uint32_t video_payload_bytes;
   uint8_t first_video_stream_id;
   bool has_video_payload;
   bool has_sequence_header;
   uint16_t sequence_width;
   uint16_t sequence_height;
   uint8_t aspect_ratio_code;
   uint8_t frame_rate_code;
   uint32_t bit_rate_value;
   uint16_t vbv_buffer_size_value;
   bool constrained_parameters_flag;
};

struct deevee_dvd_menu_button
{
   uint8_t number;
   uint8_t color_table;
   bool auto_action;
   uint16_t x_start;
   uint16_t x_end;
   uint16_t y_start;
   uint16_t y_end;
   uint8_t up;
   uint8_t down;
   uint8_t left;
   uint8_t right;
   uint8_t command[8];
};

struct deevee_dvd_menu_render_probe
{
   uint32_t scanned_bytes;
   uint32_t video_pes_packets;
   uint32_t private_stream_1_packets;
   uint32_t private_stream_2_packets;
   uint32_t subpicture_packets;
   uint32_t nav_pci_packets;
   uint32_t nav_dsi_packets;
   uint8_t first_subpicture_stream_id;
   uint8_t starting_button;
   uint8_t forced_select_button;
   uint8_t forced_action_button;
   uint8_t button_count;
   uint16_t pre_command_count;
   uint16_t post_command_count;
   uint16_t cell_command_count;
   uint8_t parsed_post_command_count;
   uint8_t post_commands[DEEVEE_DVD_MAX_PROBED_COMMANDS][8];
   struct deevee_dvd_menu_button buttons[DEEVEE_DVD_MAX_MENU_BUTTONS];
   uint32_t subpicture_clut[16];
   uint32_t select_color_table[3];
   bool has_subpicture_clut;
   bool has_select_color_table;
   bool has_video;
   bool has_subpicture;
   bool has_nav;
};

enum deevee_dvd_playback_target_type
{
   DEEVEE_DVD_PLAYBACK_TARGET_NONE = 0,
   DEEVEE_DVD_PLAYBACK_TARGET_TITLE,
   DEEVEE_DVD_PLAYBACK_TARGET_MENU
};

enum deevee_dvd_menu_domain
{
   DEEVEE_DVD_MENU_DOMAIN_NONE = 0,
   DEEVEE_DVD_MENU_DOMAIN_VMG,
   DEEVEE_DVD_MENU_DOMAIN_VTS
};

struct deevee_dvd_playback_target
{
   enum deevee_dvd_playback_target_type type;
   enum deevee_dvd_menu_domain menu_domain;
   uint8_t title_number;
   uint8_t vts_number;
   uint8_t vts_title_number;
   uint16_t ptt_number;
   uint16_t pgc_number;
};

struct deevee_dvd_title_pgc
{
   uint8_t vts_number;
   uint8_t vts_title_number;
   uint16_t ptt_number;
   uint16_t pgc_number;
   uint16_t program_number;
   uint8_t program_count;
   uint8_t cell_count;
   uint8_t first_cell;
   uint8_t last_cell;
   uint32_t first_sector;
   uint32_t last_sector;
   uint32_t sector_count;
};

struct deevee_dvd_packet
{
   uint8_t stream_id;
   const uint8_t *payload;
   size_t payload_size;
   uint32_t logical_sector;
   unsigned vob_index;
   bool has_scr;
   uint64_t scr;
   bool has_pts;
   uint64_t pts;
   bool has_dts;
   uint64_t dts;
};

typedef bool (*deevee_dvd_video_payload_callback)(const uint8_t *payload,
      size_t payload_size, void *user_data);
typedef bool (*deevee_dvd_packet_callback)(
      const struct deevee_dvd_packet *packet, void *user_data);

enum deevee_dvd_status deevee_dvd_probe(struct deevee_disc *disc,
      struct deevee_dvd_info *info);
enum deevee_dvd_status deevee_dvd_read_title_table(struct deevee_disc *disc,
      const struct deevee_dvd_info *info,
      struct deevee_dvd_title_table *table);
enum deevee_dvd_status deevee_dvd_read_menu_language_table(
      struct deevee_disc *disc, const struct deevee_dvd_info *info,
      struct deevee_dvd_menu_language_table *table);
enum deevee_dvd_status deevee_dvd_read_first_menu_pgc(
      struct deevee_disc *disc, const struct deevee_dvd_info *info,
      struct deevee_dvd_menu_pgc_summary *pgc);
enum deevee_dvd_status deevee_dvd_read_first_menu_pgc_tables(
      struct deevee_disc *disc, const struct deevee_dvd_info *info,
      struct deevee_dvd_menu_pgc_table_summary *tables);
enum deevee_dvd_status deevee_dvd_resolve_first_menu_vob_span(
      struct deevee_disc *disc, const struct deevee_dvd_info *info,
      struct deevee_dvd_menu_vob_span *span);
enum deevee_dvd_status deevee_dvd_probe_first_menu_vob_packets(
      struct deevee_disc *disc, const struct deevee_dvd_info *info,
      struct deevee_dvd_vob_packet_probe *probe);
enum deevee_dvd_status deevee_dvd_probe_first_menu_video(
      struct deevee_disc *disc, const struct deevee_dvd_info *info,
      struct deevee_dvd_video_probe *probe);
enum deevee_dvd_status deevee_dvd_walk_first_menu_video_payloads(
      struct deevee_disc *disc, const struct deevee_dvd_info *info,
      deevee_dvd_video_payload_callback callback, void *user_data);
enum deevee_dvd_status deevee_dvd_walk_vob_video_payloads(
      struct deevee_disc *disc, const char *iso_path,
      deevee_dvd_video_payload_callback callback, void *user_data);
enum deevee_dvd_status deevee_dvd_walk_vob_packets(
      struct deevee_disc *disc, const char *iso_path,
      deevee_dvd_packet_callback callback, void *user_data);
enum deevee_dvd_status deevee_dvd_walk_vts_menu_pgc_video_payloads(
      struct deevee_disc *disc, unsigned vts_number, unsigned pgc_index,
      deevee_dvd_video_payload_callback callback, void *user_data);
enum deevee_dvd_status deevee_dvd_walk_vts_menu_pgc_packets(
      struct deevee_disc *disc, unsigned vts_number, unsigned pgc_index,
      deevee_dvd_packet_callback callback, void *user_data);
enum deevee_dvd_status deevee_dvd_walk_vmgm_menu_pgc_video_payloads(
      struct deevee_disc *disc, unsigned pgc_index,
      deevee_dvd_video_payload_callback callback, void *user_data);
enum deevee_dvd_status deevee_dvd_walk_vmgm_menu_pgc_packets(
      struct deevee_disc *disc, unsigned pgc_index,
      deevee_dvd_packet_callback callback, void *user_data);
enum deevee_dvd_status deevee_dvd_probe_vts_menu_pgc_render_streams(
      struct deevee_disc *disc, unsigned vts_number, unsigned pgc_index,
      struct deevee_dvd_menu_render_probe *probe);
enum deevee_dvd_status deevee_dvd_probe_vmgm_menu_pgc_render_streams(
      struct deevee_disc *disc, unsigned pgc_index,
      struct deevee_dvd_menu_render_probe *probe);
enum deevee_dvd_status deevee_dvd_probe_vob_menu_render_streams(
      struct deevee_disc *disc, const char *iso_path,
      struct deevee_dvd_menu_render_probe *probe);
bool deevee_dvd_decode_playback_target_command(const uint8_t command[8],
      unsigned current_vts, enum deevee_dvd_menu_domain current_menu_domain,
      const struct deevee_dvd_title_table *title_table,
      struct deevee_dvd_playback_target *target);
const char *deevee_dvd_playback_target_type_name(
      enum deevee_dvd_playback_target_type type);
const char *deevee_dvd_menu_domain_name(enum deevee_dvd_menu_domain domain);
enum deevee_dvd_status deevee_dvd_resolve_title_pgc(
      struct deevee_disc *disc,
      const struct deevee_dvd_playback_target *target,
      struct deevee_dvd_title_pgc *title_pgc);
enum deevee_dvd_status deevee_dvd_walk_title_pgc_video_payloads(
      struct deevee_disc *disc, const struct deevee_dvd_title_pgc *title_pgc,
      deevee_dvd_video_payload_callback callback, void *user_data);
enum deevee_dvd_status deevee_dvd_walk_title_pgc_packets(
      struct deevee_disc *disc, const struct deevee_dvd_title_pgc *title_pgc,
      deevee_dvd_packet_callback callback, void *user_data);
enum deevee_dvd_status deevee_dvd_walk_packets_in_buffer(
      const uint8_t *data, size_t data_size, deevee_dvd_packet_callback callback,
      void *user_data);
const char *deevee_dvd_status_name(enum deevee_dvd_status status);

#endif
