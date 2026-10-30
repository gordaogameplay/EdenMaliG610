// SPDX-FileCopyrightText: Copyright 2026 gordaogameplay
// SPDX-License-Identifier: GPL-3.0-or-later
// EdenMaliG610 — os comandos do motor Maxwell 3D.
//
// Gerado por `tools/extrair_comandos_maxwell.py` — não editar à mão.
//
// A fonte da verdade é o upstream (`src/video_core/engines/maxwell_3d.h`),
// e os índices saem de `offsetof(Regs, campo) / 4` — a mesma macro que o
// Eden usa (`MAXWELL3D_REG_INDEX`). Compilados contra o cabeçalho do
// upstream, não adivinhados.
//
// O ID de um método **é** o índice do registo: no push buffer, o
// cabeçalho do método traz esse ID nos bits 0..12 (ver
// `g610/gpu/pushbuffer.hpp`).
#pragma once

#include <cstdint>

namespace g610::gpu {

// Os comandos do Maxwell 3D (o índice do registo no upstream).
namespace maxwell3d {
inline constexpr std::uint32_t nop = 64;
inline constexpr std::uint32_t wait_for_idle = 68;
inline constexpr std::uint32_t load_mme_instruction_ptr = 69;
inline constexpr std::uint32_t load_mme_instruction = 70;
inline constexpr std::uint32_t load_mme_start_address = 72;
inline constexpr std::uint32_t shadow_ram_control = 73;
inline constexpr std::uint32_t go_idle = 79;
inline constexpr std::uint32_t trigger = 80;
inline constexpr std::uint32_t trigger_wfi = 81;
inline constexpr std::uint32_t instrumentation_method_header = 84;
inline constexpr std::uint32_t instrumentation_method_data = 85;
inline constexpr std::uint32_t launch_dma = 108;
inline constexpr std::uint32_t inline_data = 109;
inline constexpr std::uint32_t run_ds_now = 128;
inline constexpr std::uint32_t aliased_line_width_enabled = 131;
inline constexpr std::uint32_t mandated_early_z = 132;
inline constexpr std::uint32_t sync_info = 178;
inline constexpr std::uint32_t prim_circular_buffer_throttle = 180;
inline constexpr std::uint32_t flush_invalidate_rop_mini_cache = 181;
inline constexpr std::uint32_t alpha_circular_buffer_size = 183;
inline constexpr std::uint32_t peer_semaphore_release = 188;
inline constexpr std::uint32_t iterated_blend_optimization = 189;
inline constexpr std::uint32_t draw_zero_index = 193;
inline constexpr std::uint32_t render_enable_control_load_const_buffer = 195;
inline constexpr std::uint32_t ieee_clean_update = 197;
inline constexpr std::uint32_t max_patches_per_batch = 222;
inline constexpr std::uint32_t rasterize_enable = 223;
inline constexpr std::uint32_t raster_input = 464;
inline constexpr std::uint32_t transform_feedback_enabled = 465;
inline constexpr std::uint32_t primitive_restart_topology_change_enable = 466;
inline constexpr std::uint32_t alpha_fraction = 467;
inline constexpr std::uint32_t color_zero_bandwidth_clear = 489;
inline constexpr std::uint32_t z_zero_bandwidth_clear = 490;
inline constexpr std::uint32_t isbe_save_restore_program_offset = 491;
inline constexpr std::uint32_t max_geometry_instances_per_task = 856;
inline constexpr std::uint32_t vertex_buffer_first = 861;
inline constexpr std::uint32_t vertex_buffer_count = 862;
inline constexpr std::uint32_t shader_cache_icache_prefetch = 869;
inline constexpr std::uint32_t force_transition_to_beta = 870;
inline constexpr std::uint32_t reduce_colour_thresholds = 871;
inline constexpr std::uint32_t clear_stencil = 872;
inline constexpr std::uint32_t polygon_smooth = 877;
inline constexpr std::uint32_t zeta_mark_clean_ieee = 878;
inline constexpr std::uint32_t polygon_offset_point_enable = 880;
inline constexpr std::uint32_t polygon_offset_line_enable = 881;
inline constexpr std::uint32_t polygon_offset_fill_enable = 882;
inline constexpr std::uint32_t patch_vertices = 883;
inline constexpr std::uint32_t fragment_barrier = 888;
inline constexpr std::uint32_t sm_timeout = 889;
inline constexpr std::uint32_t primitive_restart_array = 890;
inline constexpr std::uint32_t window_offset_x = 894;
inline constexpr std::uint32_t window_offset_y = 895;
inline constexpr std::uint32_t select_texture_headers = 964;
inline constexpr std::uint32_t pm_local_trigger = 966;
inline constexpr std::uint32_t post_z_pixel_imask = 967;
inline constexpr std::uint32_t stencil_back_ref = 981;
inline constexpr std::uint32_t stencil_back_mask = 982;
inline constexpr std::uint32_t stencil_back_func_mask = 983;
inline constexpr std::uint32_t invalidate_texture_data_cache = 989;
inline constexpr std::uint32_t tiled_cache_barrier = 991;
inline constexpr std::uint32_t line_mode_clip_generated_edge_do_not_draw = 995;
inline constexpr std::uint32_t color_mask_common = 996;
inline constexpr std::uint32_t color_target_mrt_enable = 1003;
inline constexpr std::uint32_t blend_control_allow_float_pixel_kills = 1015;
inline constexpr std::uint32_t tiled_cache_treat_heavy_as_light = 1023;
inline constexpr std::uint32_t tessellation_cut_height = 1026;
inline constexpr std::uint32_t max_gs_instances_per_task = 1027;
inline constexpr std::uint32_t max_gs_output_vertices_per_task = 1028;
inline constexpr std::uint32_t reserved_sw_method0 = 1029;
inline constexpr std::uint32_t gs_output_cb_storage_multiplier = 1030;
inline constexpr std::uint32_t beta_cb_storage_constant = 1031;
inline constexpr std::uint32_t ti_output_cb_storage_multiplier = 1032;
inline constexpr std::uint32_t alpha_cb_storage_constraint = 1033;
inline constexpr std::uint32_t reserved_sw_method1 = 1034;
inline constexpr std::uint32_t reserved_sw_method2 = 1035;
inline constexpr std::uint32_t draw_texture_src_y0 = 1067;
inline constexpr std::uint32_t unbind_all_constant_buffers = 1085;
inline constexpr std::uint32_t reserved_sw_method14 = 1088;
inline constexpr std::uint32_t reserved_sw_method15 = 1089;
inline constexpr std::uint32_t no_operation_data_high = 1091;
inline constexpr std::uint32_t depth_bias_control = 1092;
inline constexpr std::uint32_t pm_trigger_end = 1093;
inline constexpr std::uint32_t vertex_id_base = 1094;
inline constexpr std::uint32_t stencil_compression_enabled = 1095;
inline constexpr std::uint32_t mutable_method_treat_mutable_as_heavy = 1101;
inline constexpr std::uint32_t post_ps_use_pre_ps_coverage = 1102;
inline constexpr std::uint32_t blend_per_format_snorm8_unorm16_snorm16_enabled = 1104;
inline constexpr std::uint32_t flush_pending_writes_sm_gloal_store = 1105;
inline constexpr std::uint32_t conservative_raster_enable = 1106;
inline constexpr std::uint32_t offset_render_target_index_by_viewport_index = 1148;
inline constexpr std::uint32_t force_heavy_method_sync = 1149;
inline constexpr std::uint32_t invalidate_sampler_cache = 1155;
inline constexpr std::uint32_t invalidate_texture_header_cache = 1156;
inline constexpr std::uint32_t vertex_array_instance_first = 1157;
inline constexpr std::uint32_t vertex_array_instance_subsequent = 1158;
inline constexpr std::uint32_t draw_auto_byte_count = 1167;
inline constexpr std::uint32_t circular_buffer_size = 1184;
inline constexpr std::uint32_t primitive_restart_topology_change_index = 1193;
inline constexpr std::uint32_t depth_test_enable = 1203;
inline constexpr std::uint32_t blend_per_target_enabled = 1209;
inline constexpr std::uint32_t depth_write_enabled = 1210;
inline constexpr std::uint32_t alpha_test_enabled = 1211;
inline constexpr std::uint32_t inline_index_4x8_index0 = 1217;
inline constexpr std::uint32_t draw_auto_stride = 1222;
inline constexpr std::uint32_t stencil_enable = 1248;
inline constexpr std::uint32_t stencil_front_ref = 1253;
inline constexpr std::uint32_t stencil_front_func_mask = 1254;
inline constexpr std::uint32_t stencil_front_mask = 1255;
inline constexpr std::uint32_t draw_auto_start_byte_count = 1257;
inline constexpr std::uint32_t line_override_multisample = 1286;
inline constexpr std::uint32_t alpha_hysteresis_rounds = 1288;
inline constexpr std::uint32_t global_base_vertex_index = 1293;
inline constexpr std::uint32_t global_base_instance_index = 1294;
inline constexpr std::uint32_t store_zcull = 1305;
inline constexpr std::uint32_t load_zcull = 1344;
inline constexpr std::uint32_t surface_clip_id_height = 1345;
inline constexpr std::uint32_t zpass_pixel_count_enable = 1349;
inline constexpr std::uint32_t zcull_stats_enable = 1351;
inline constexpr std::uint32_t point_sprite_enable = 1352;
inline constexpr std::uint32_t shader_exceptions_enable = 1354;
inline constexpr std::uint32_t clear_report_value = 1356;
inline constexpr std::uint32_t anti_alias_enable = 1357;
inline constexpr std::uint32_t zeta_enable = 1358;
inline constexpr std::uint32_t render_enable_mode = 1366;
inline constexpr std::uint32_t line_anti_alias_enable = 1372;
inline constexpr std::uint32_t active_zcull_region_id = 1380;
inline constexpr std::uint32_t stencil_two_side_enable = 1381;
inline constexpr std::uint32_t framebuffer_srgb = 1390;
inline constexpr std::uint32_t anti_alias_samples_mode = 1396;
inline constexpr std::uint32_t edge_flag = 1401;
inline constexpr std::uint32_t draw_inline_index = 1402;
inline constexpr std::uint32_t inline_index_2x16_even = 1404;
inline constexpr std::uint32_t draw_end = 1413;
inline constexpr std::uint32_t draw_begin = 1414;
inline constexpr std::uint32_t add_to_primitive_id = 1416;
inline constexpr std::uint32_t load_to_primitive_id = 1417;
inline constexpr std::uint32_t anti_alias_point_enable = 1430;
inline constexpr std::uint32_t line_stipple_enable = 1435;
inline constexpr std::uint32_t two_sided_light_enabled = 1442;
inline constexpr std::uint32_t polygon_stipple_enabled = 1443;
inline constexpr std::uint32_t zeta_layer_offset = 1511;
inline constexpr std::uint32_t index_buffer_first = 1527;
inline constexpr std::uint32_t index_buffer_count = 1528;
inline constexpr std::uint32_t index_buffer32_first = 1529;
inline constexpr std::uint32_t index_buffer16_first = 1530;
inline constexpr std::uint32_t index_buffer8_first = 1531;
inline constexpr std::uint32_t index_buffer32_subsequent = 1532;
inline constexpr std::uint32_t index_buffer16_subsequent = 1533;
inline constexpr std::uint32_t index_buffer8_subsequent = 1534;
inline constexpr std::uint32_t gl_cull_test_enabled = 1606;
inline constexpr std::uint32_t viewport_scale_offset_enabled = 1611;
inline constexpr std::uint32_t topology_override = 1628;
inline constexpr std::uint32_t zcull_sync = 1630;
inline constexpr std::uint32_t clip_id_test_enable = 1631;
inline constexpr std::uint32_t surface_clip_id_width = 1632;
inline constexpr std::uint32_t clip_id = 1633;
inline constexpr std::uint32_t depth_bounds_enable = 1647;
inline constexpr std::uint32_t blend_float_zero_times_anything_is_zero = 1648;
inline constexpr std::uint32_t z_compression_enable = 1651;
inline constexpr std::uint32_t clear_surface = 1652;
inline constexpr std::uint32_t clear_clip_id_surface = 1653;
inline constexpr std::uint32_t pipe_nop = 1675;
inline constexpr std::uint32_t report_semaphore_query = 1731;
inline constexpr std::uint32_t falcon = 2240;
inline constexpr std::uint32_t const_buffer_buffer = 2276;
inline constexpr std::uint32_t color_clamp_enable = 2432;
inline constexpr std::uint32_t bindless_texture_const_buffer_slot = 2434;
inline constexpr std::uint32_t trap_handler = 2435;
}  // namespace maxwell3d

}  // namespace g610::gpu
