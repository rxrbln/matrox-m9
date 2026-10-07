/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright 2026 René Rebe <rene@exactco.de> */
/* Generated from actual init-only caller boundary + independent queue flush. */
#define M9_FAA_BLOB_BYTES 122288u
#define M9_FAA_BLOB_FNV 0xeaf9b1e5u
#define M9_FAA_PRIMARY_OWNER 0x1au
#define M9_FAA_PRIMARY_BYTES 0x1310u
#define M9_FAA_COLOR_OWNER 0x1u
#define M9_FAA_OUTPUT_OWNER 0xffffffffu
#define M9_FAA_TRACKER_LAST 0xb0u
#define M9_FAA_CLEAR_OWNER 0x4u
#define M9_FAA_CLEAR_BYTES 0x97000u
#define M9_FAA_CLEAR_START 0x11d8u
#define M9_FAA_CLEAR_END 0x1224u
#define M9_FAA_PHASE_CONTROL 1
#define M9_FAA_HAS_OUTPUT 0
#define M9_FAA_PHASE_INIT_ONLY 1
#define M9_FAA_GRAPH_INIT_CONTROL 1
#define M9_FAA_STARTUP_LOCAL_OWNER 27u
#define M9_FAA_STARTUP_LOCAL_BYTES 0x80000u
#define M9_FAA_COUNTER_VIRTUAL 1
static const struct m9_faa_owner_spec m9_faa_owner_specs[] = {
	{ 0xc1a000u, 0x100u, true, false, false, 0x0u },
	{ 0xf19000u, 0x50000u, true, false, false, 0x0u },
	{ 0xf69000u, 0x136080u, true, false, false, 0x0u },
	{ 0x10a0000u, 0x13080u, true, false, false, 0x0u },
	{ 0x10b9000u, 0x98000u, true, false, false, 0x0u },
	{ 0x1151000u, 0x8000u, true, false, false, 0x0u },
	{ 0x40808000u, 0x50000u, false, true, true, 0x50000u },
	{ 0x1159000u, 0x8000u, true, false, false, 0x0u },
	{ 0x1161000u, 0x80u, true, false, false, 0x0u },
	{ 0x1169000u, 0x8000u, true, false, false, 0x0u },
	{ 0x1171000u, 0x8000u, true, false, false, 0x0u },
	{ 0x1179000u, 0x20080u, true, false, false, 0x0u },
	{ 0x119a000u, 0x1d080u, true, false, false, 0x0u },
	{ 0x11b8000u, 0x1080u, true, false, false, 0x0u },
	{ 0x11ba000u, 0x1080u, true, false, false, 0x0u },
	{ 0x11c9000u, 0x8000u, true, false, false, 0x0u },
	{ 0x11c1000u, 0x8000u, true, false, false, 0x0u },
	{ 0x11d9000u, 0x8000u, true, false, false, 0x0u },
	{ 0x11d1000u, 0x8000u, true, false, false, 0x0u },
	{ 0x11e1000u, 0x40080u, true, false, false, 0x0u },
	{ 0x1222000u, 0x5080u, true, false, false, 0x0u },
	{ 0x1228000u, 0x80u, true, false, false, 0x0u },
	{ 0x1229000u, 0x580u, true, false, false, 0x0u },
	{ 0x122a000u, 0x460080u, true, false, false, 0x0u },
	{ 0x1691000u, 0x408000u, true, false, false, 0x0u },
	{ 0x1a99000u, 0x100080u, true, false, false, 0x0u },
	{ 0x40002000u, 0x40000u, false, true, true, 0x40000u },
	{ 0xe99000u, 0x80000u, true, false, false, 0x0u },
};
static const struct m9_faa_span m9_faa_spans[] = {
	{ 8u, 0x0u, 0x0u, 0x14u },
	{ 12u, 0x0u, 0x14u, 0x1c1d8u },
	{ 13u, 0x0u, 0x1c1ecu, 0x80u },
	{ 14u, 0x0u, 0x1c26cu, 0x20u },
	{ 15u, 0x0u, 0x1c28cu, 0x400u },
	{ 17u, 0x0u, 0x1c68cu, 0x400u },
	{ 21u, 0x0u, 0x1ca8cu, 0x14u },
	{ 26u, 0x0u, 0x1caa0u, 0x1310u },
};
static const struct m9_faa_fixup m9_faa_fixups[] = {
	{ 26u, 0x470u, 5u, 0x0u, 0x24930u, 1u, 0x1151001u },
	{ 26u, 0x1184u, 7u, 0x0u, 0x24930u, 1u, 0x1159001u },
	{ 26u, 0x1248u, 10u, 0x0u, 0x24930u, 1u, 0x1171001u },
	{ 26u, 0x12b8u, 11u, 0x0u, 0x24930u, 1u, 0x1179001u },
};
static const struct m9_faa_tracker m9_faa_trackers[] = {
	{ 26u, 0x8u, 0x21u },
	{ 26u, 0x5cu, 0x31u },
	{ 26u, 0x124u, 0x30u },
	{ 26u, 0x430u, 0x41u },
	{ 26u, 0x468u, 0x51u },
	{ 26u, 0x4f0u, 0x50u },
	{ 26u, 0xfbcu, 0x50u },
	{ 26u, 0x117cu, 0x61u },
	{ 26u, 0x122cu, 0x71u },
	{ 26u, 0x1240u, 0x81u },
	{ 26u, 0x1280u, 0x91u },
	{ 26u, 0x12b0u, 0xa1u },
	{ 26u, 0x130cu, 0xb1u },
};
