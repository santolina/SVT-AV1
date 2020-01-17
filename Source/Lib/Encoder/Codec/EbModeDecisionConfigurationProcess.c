/*
* Copyright(c) 2019 Intel Corporation
* SPDX - License - Identifier: BSD - 2 - Clause - Patent
*/

/*
* Copyright (c) 2016, Alliance for Open Media. All rights reserved
*
* This source code is subject to the terms of the BSD 2 Clause License and
* the Alliance for Open Media Patent License 1.0. If the BSD 2 Clause License
* was not distributed with this source code in the LICENSE file, you can
* obtain it at www.aomedia.org/license/software. If the Alliance for Open
* Media Patent License 1.0 was not distributed with this source code in the
* PATENTS file, you can obtain it at www.aomedia.org/license/patent.
*/

#include <stdlib.h>

#include "EbEncHandle.h"
#include "EbUtility.h"
#include "EbPictureControlSet.h"
#include "EbModeDecisionConfigurationProcess.h"
#include "EbRateControlResults.h"
#include "EbEncDecTasks.h"
#include "EbModeDecisionConfiguration.h"
#include "EbReferenceObject.h"
#include "EbModeDecisionProcess.h"
#include "av1me.h"
#include "EbCommonUtils.h"
#include "EbQMatrices.h"
#include "EbLog.h"

#define MAX_MESH_SPEED 5 // Max speed setting for mesh motion method
static MeshPattern good_quality_mesh_patterns[MAX_MESH_SPEED + 1][MAX_MESH_STEP] = {
    {{64, 8}, {28, 4}, {15, 1}, {7, 1}},
    {{64, 8}, {28, 4}, {15, 1}, {7, 1}},
    {{64, 8}, {14, 2}, {7, 1}, {7, 1}},
    {{64, 16}, {24, 8}, {12, 4}, {7, 1}},
    {{64, 16}, {24, 8}, {12, 4}, {7, 1}},
    {{64, 16}, {24, 8}, {12, 4}, {7, 1}},
};
static unsigned char good_quality_max_mesh_pct[MAX_MESH_SPEED + 1] = {50, 50, 25, 15, 5, 1};
// TODO: These settings are pretty relaxed, tune them for
// each speed setting
static MeshPattern intrabc_mesh_patterns[MAX_MESH_SPEED + 1][MAX_MESH_STEP] = {
    {{256, 1}, {256, 1}, {0, 0}, {0, 0}},
    {{256, 1}, {256, 1}, {0, 0}, {0, 0}},
    {{64, 1}, {64, 1}, {0, 0}, {0, 0}},
    {{64, 1}, {64, 1}, {0, 0}, {0, 0}},
    {{64, 4}, {16, 1}, {0, 0}, {0, 0}},
    {{64, 4}, {16, 1}, {0, 0}, {0, 0}},
};
static uint8_t intrabc_max_mesh_pct[MAX_MESH_SPEED + 1] = {100, 100, 100, 25, 25, 10};

// Adaptive Depth Partitioning
// Shooting states
#define UNDER_SHOOTING 0
#define OVER_SHOOTING 1
#define TBD_SHOOTING 2

#define SB_PRED_OPEN_LOOP_COST 100 // Let's assume PRED_OPEN_LOOP_COST costs ~100 U
#define U_101 101
#define U_102 102
#define U_103 103
#define U_104 104
#define U_105 105
#define U_107 107
#define SB_FAST_OPEN_LOOP_COST 108
#define U_109 109
#define SB_OPEN_LOOP_COST 110 // F_MDC is ~10% slower than PRED_OPEN_LOOP_COST
#define U_111 111
#define U_112 112
#define U_113 113
#define U_114 114
#define U_115 115
#define U_116 116
#define U_117 117
#define U_118 118
#define U_119 119
#define U_120 120
#define U_121 121
#define U_122 122
#define U_125 125
#define U_127 127
#define U_130 130
#define U_132 132
#define U_133 133
#define U_134 134
#define U_140 140
#define U_145 145
#define U_150 150
#define U_152 152
#define SQ_NON4_BLOCKS_SEARCH_COST 155
#define SQ_BLOCKS_SEARCH_COST 190
#define HIGH_SB_SCORE 60000
#define MEDIUM_SB_SCORE 16000
#define LOW_SB_SCORE 6000
#define MAX_LUMINOSITY_BOOST 10
int32_t budget_per_sb_boost[MAX_SUPPORTED_MODES] = {55, 55, 55, 55, 55, 55, 5, 5, 0, 0, 0, 0, 0};

// Coefficient scaling and quantization with AV1 TX are tailored to
// the AV1 TX transforms.  Regardless of the bit-depth of the input,
// the transform stages scale the coefficient values up by a factor of
// 8 (3 bits) over the scale of the pixel values.  Thus, for 8-bit
// input, the coefficients have effectively 11 bits of scale depth
// (8+3), 10-bit input pixels result in 13-bit coefficient depth
// (10+3) and 12-bit pixels yield 15-bit (12+3) coefficient depth.
// All quantizers are built using this invariant of x8, 3-bit scaling,
// thus the Q3 suffix.

// A partial exception to this rule is large transforms; to avoid
// overflow, TX blocks with > 256 pels (>16x16) are scaled only
// 4-times unity (2 bits) over the pixel depth, and TX blocks with
// over 1024 pixels (>32x32) are scaled up only 2x unity (1 bit).
// This descaling is found via av1_tx_get_scale().  Thus, 16x32, 32x16
// and 32x32 transforms actually return Q2 coefficients, and 32x64,
// 64x32 and 64x64 transforms return Q1 coefficients.  However, the
// quantizers are de-scaled down on-the-fly by the same amount
// (av1_tx_get_scale()) during quantization, and as such the
// dequantized/decoded coefficients, even for large TX blocks, are always
// effectively Q3. Meanwhile, quantized/coded coefficients are Q0
// because Qn quantizers are applied to Qn tx coefficients.

// Note that encoder decision making (which uses the quantizer to
// generate several bespoke lamdas for RDO and other heuristics)
// expects quantizers to be larger for higher-bitdepth input.  In
// addition, the minimum allowable quantizer is 4; smaller values will
// underflow to 0 in the actual quantization routines.

static const int16_t dc_qlookup_q3[QINDEX_RANGE] = {
    4,   8,   8,   9,   10,  11,  12,  12,  13,   14,   15,   16,   17,   18,   19,   19,
    20,  21,  22,  23,  24,  25,  26,  26,  27,   28,   29,   30,   31,   32,   32,   33,
    34,  35,  36,  37,  38,  38,  39,  40,  41,   42,   43,   43,   44,   45,   46,   47,
    48,  48,  49,  50,  51,  52,  53,  53,  54,   55,   56,   57,   57,   58,   59,   60,
    61,  62,  62,  63,  64,  65,  66,  66,  67,   68,   69,   70,   70,   71,   72,   73,
    74,  74,  75,  76,  77,  78,  78,  79,  80,   81,   81,   82,   83,   84,   85,   85,
    87,  88,  90,  92,  93,  95,  96,  98,  99,   101,  102,  104,  105,  107,  108,  110,
    111, 113, 114, 116, 117, 118, 120, 121, 123,  125,  127,  129,  131,  134,  136,  138,
    140, 142, 144, 146, 148, 150, 152, 154, 156,  158,  161,  164,  166,  169,  172,  174,
    177, 180, 182, 185, 187, 190, 192, 195, 199,  202,  205,  208,  211,  214,  217,  220,
    223, 226, 230, 233, 237, 240, 243, 247, 250,  253,  257,  261,  265,  269,  272,  276,
    280, 284, 288, 292, 296, 300, 304, 309, 313,  317,  322,  326,  330,  335,  340,  344,
    349, 354, 359, 364, 369, 374, 379, 384, 389,  395,  400,  406,  411,  417,  423,  429,
    435, 441, 447, 454, 461, 467, 475, 482, 489,  497,  505,  513,  522,  530,  539,  549,
    559, 569, 579, 590, 602, 614, 626, 640, 654,  668,  684,  700,  717,  736,  755,  775,
    796, 819, 843, 869, 896, 925, 955, 988, 1022, 1058, 1098, 1139, 1184, 1232, 1282, 1336,
};

static const int16_t dc_qlookup_10_q3[QINDEX_RANGE] = {
    4,    9,    10,   13,   15,   17,   20,   22,   25,   28,   31,   34,   37,   40,   43,   47,
    50,   53,   57,   60,   64,   68,   71,   75,   78,   82,   86,   90,   93,   97,   101,  105,
    109,  113,  116,  120,  124,  128,  132,  136,  140,  143,  147,  151,  155,  159,  163,  166,
    170,  174,  178,  182,  185,  189,  193,  197,  200,  204,  208,  212,  215,  219,  223,  226,
    230,  233,  237,  241,  244,  248,  251,  255,  259,  262,  266,  269,  273,  276,  280,  283,
    287,  290,  293,  297,  300,  304,  307,  310,  314,  317,  321,  324,  327,  331,  334,  337,
    343,  350,  356,  362,  369,  375,  381,  387,  394,  400,  406,  412,  418,  424,  430,  436,
    442,  448,  454,  460,  466,  472,  478,  484,  490,  499,  507,  516,  525,  533,  542,  550,
    559,  567,  576,  584,  592,  601,  609,  617,  625,  634,  644,  655,  666,  676,  687,  698,
    708,  718,  729,  739,  749,  759,  770,  782,  795,  807,  819,  831,  844,  856,  868,  880,
    891,  906,  920,  933,  947,  961,  975,  988,  1001, 1015, 1030, 1045, 1061, 1076, 1090, 1105,
    1120, 1137, 1153, 1170, 1186, 1202, 1218, 1236, 1253, 1271, 1288, 1306, 1323, 1342, 1361, 1379,
    1398, 1416, 1436, 1456, 1476, 1496, 1516, 1537, 1559, 1580, 1601, 1624, 1647, 1670, 1692, 1717,
    1741, 1766, 1791, 1817, 1844, 1871, 1900, 1929, 1958, 1990, 2021, 2054, 2088, 2123, 2159, 2197,
    2236, 2276, 2319, 2363, 2410, 2458, 2508, 2561, 2616, 2675, 2737, 2802, 2871, 2944, 3020, 3102,
    3188, 3280, 3375, 3478, 3586, 3702, 3823, 3953, 4089, 4236, 4394, 4559, 4737, 4929, 5130, 5347,
};

static const int16_t dc_qlookup_12_q3[QINDEX_RANGE] = {
    4,     12,    18,    25,    33,    41,    50,    60,    70,    80,    91,    103,   115,
    127,   140,   153,   166,   180,   194,   208,   222,   237,   251,   266,   281,   296,
    312,   327,   343,   358,   374,   390,   405,   421,   437,   453,   469,   484,   500,
    516,   532,   548,   564,   580,   596,   611,   627,   643,   659,   674,   690,   706,
    721,   737,   752,   768,   783,   798,   814,   829,   844,   859,   874,   889,   904,
    919,   934,   949,   964,   978,   993,   1008,  1022,  1037,  1051,  1065,  1080,  1094,
    1108,  1122,  1136,  1151,  1165,  1179,  1192,  1206,  1220,  1234,  1248,  1261,  1275,
    1288,  1302,  1315,  1329,  1342,  1368,  1393,  1419,  1444,  1469,  1494,  1519,  1544,
    1569,  1594,  1618,  1643,  1668,  1692,  1717,  1741,  1765,  1789,  1814,  1838,  1862,
    1885,  1909,  1933,  1957,  1992,  2027,  2061,  2096,  2130,  2165,  2199,  2233,  2267,
    2300,  2334,  2367,  2400,  2434,  2467,  2499,  2532,  2575,  2618,  2661,  2704,  2746,
    2788,  2830,  2872,  2913,  2954,  2995,  3036,  3076,  3127,  3177,  3226,  3275,  3324,
    3373,  3421,  3469,  3517,  3565,  3621,  3677,  3733,  3788,  3843,  3897,  3951,  4005,
    4058,  4119,  4181,  4241,  4301,  4361,  4420,  4479,  4546,  4612,  4677,  4742,  4807,
    4871,  4942,  5013,  5083,  5153,  5222,  5291,  5367,  5442,  5517,  5591,  5665,  5745,
    5825,  5905,  5984,  6063,  6149,  6234,  6319,  6404,  6495,  6587,  6678,  6769,  6867,
    6966,  7064,  7163,  7269,  7376,  7483,  7599,  7715,  7832,  7958,  8085,  8214,  8352,
    8492,  8635,  8788,  8945,  9104,  9275,  9450,  9639,  9832,  10031, 10245, 10465, 10702,
    10946, 11210, 11482, 11776, 12081, 12409, 12750, 13118, 13501, 13913, 14343, 14807, 15290,
    15812, 16356, 16943, 17575, 18237, 18949, 19718, 20521, 21387,
};
static const int16_t ac_qlookup_q3[QINDEX_RANGE] = {
    4,    8,    9,    10,   11,   12,   13,   14,   15,   16,   17,   18,   19,   20,   21,   22,
    23,   24,   25,   26,   27,   28,   29,   30,   31,   32,   33,   34,   35,   36,   37,   38,
    39,   40,   41,   42,   43,   44,   45,   46,   47,   48,   49,   50,   51,   52,   53,   54,
    55,   56,   57,   58,   59,   60,   61,   62,   63,   64,   65,   66,   67,   68,   69,   70,
    71,   72,   73,   74,   75,   76,   77,   78,   79,   80,   81,   82,   83,   84,   85,   86,
    87,   88,   89,   90,   91,   92,   93,   94,   95,   96,   97,   98,   99,   100,  101,  102,
    104,  106,  108,  110,  112,  114,  116,  118,  120,  122,  124,  126,  128,  130,  132,  134,
    136,  138,  140,  142,  144,  146,  148,  150,  152,  155,  158,  161,  164,  167,  170,  173,
    176,  179,  182,  185,  188,  191,  194,  197,  200,  203,  207,  211,  215,  219,  223,  227,
    231,  235,  239,  243,  247,  251,  255,  260,  265,  270,  275,  280,  285,  290,  295,  300,
    305,  311,  317,  323,  329,  335,  341,  347,  353,  359,  366,  373,  380,  387,  394,  401,
    408,  416,  424,  432,  440,  448,  456,  465,  474,  483,  492,  501,  510,  520,  530,  540,
    550,  560,  571,  582,  593,  604,  615,  627,  639,  651,  663,  676,  689,  702,  715,  729,
    743,  757,  771,  786,  801,  816,  832,  848,  864,  881,  898,  915,  933,  951,  969,  988,
    1007, 1026, 1046, 1066, 1087, 1108, 1129, 1151, 1173, 1196, 1219, 1243, 1267, 1292, 1317, 1343,
    1369, 1396, 1423, 1451, 1479, 1508, 1537, 1567, 1597, 1628, 1660, 1692, 1725, 1759, 1793, 1828,
};

static const int16_t ac_qlookup_10_q3[QINDEX_RANGE] = {
    4,    9,    11,   13,   16,   18,   21,   24,   27,   30,   33,   37,   40,   44,   48,   51,
    55,   59,   63,   67,   71,   75,   79,   83,   88,   92,   96,   100,  105,  109,  114,  118,
    122,  127,  131,  136,  140,  145,  149,  154,  158,  163,  168,  172,  177,  181,  186,  190,
    195,  199,  204,  208,  213,  217,  222,  226,  231,  235,  240,  244,  249,  253,  258,  262,
    267,  271,  275,  280,  284,  289,  293,  297,  302,  306,  311,  315,  319,  324,  328,  332,
    337,  341,  345,  349,  354,  358,  362,  367,  371,  375,  379,  384,  388,  392,  396,  401,
    409,  417,  425,  433,  441,  449,  458,  466,  474,  482,  490,  498,  506,  514,  523,  531,
    539,  547,  555,  563,  571,  579,  588,  596,  604,  616,  628,  640,  652,  664,  676,  688,
    700,  713,  725,  737,  749,  761,  773,  785,  797,  809,  825,  841,  857,  873,  889,  905,
    922,  938,  954,  970,  986,  1002, 1018, 1038, 1058, 1078, 1098, 1118, 1138, 1158, 1178, 1198,
    1218, 1242, 1266, 1290, 1314, 1338, 1362, 1386, 1411, 1435, 1463, 1491, 1519, 1547, 1575, 1603,
    1631, 1663, 1695, 1727, 1759, 1791, 1823, 1859, 1895, 1931, 1967, 2003, 2039, 2079, 2119, 2159,
    2199, 2239, 2283, 2327, 2371, 2415, 2459, 2507, 2555, 2603, 2651, 2703, 2755, 2807, 2859, 2915,
    2971, 3027, 3083, 3143, 3203, 3263, 3327, 3391, 3455, 3523, 3591, 3659, 3731, 3803, 3876, 3952,
    4028, 4104, 4184, 4264, 4348, 4432, 4516, 4604, 4692, 4784, 4876, 4972, 5068, 5168, 5268, 5372,
    5476, 5584, 5692, 5804, 5916, 6032, 6148, 6268, 6388, 6512, 6640, 6768, 6900, 7036, 7172, 7312,
};

static const int16_t ac_qlookup_12_q3[QINDEX_RANGE] = {
    4,     13,    19,    27,    35,    44,    54,    64,    75,    87,    99,    112,   126,
    139,   154,   168,   183,   199,   214,   230,   247,   263,   280,   297,   314,   331,
    349,   366,   384,   402,   420,   438,   456,   475,   493,   511,   530,   548,   567,
    586,   604,   623,   642,   660,   679,   698,   716,   735,   753,   772,   791,   809,
    828,   846,   865,   884,   902,   920,   939,   957,   976,   994,   1012,  1030,  1049,
    1067,  1085,  1103,  1121,  1139,  1157,  1175,  1193,  1211,  1229,  1246,  1264,  1282,
    1299,  1317,  1335,  1352,  1370,  1387,  1405,  1422,  1440,  1457,  1474,  1491,  1509,
    1526,  1543,  1560,  1577,  1595,  1627,  1660,  1693,  1725,  1758,  1791,  1824,  1856,
    1889,  1922,  1954,  1987,  2020,  2052,  2085,  2118,  2150,  2183,  2216,  2248,  2281,
    2313,  2346,  2378,  2411,  2459,  2508,  2556,  2605,  2653,  2701,  2750,  2798,  2847,
    2895,  2943,  2992,  3040,  3088,  3137,  3185,  3234,  3298,  3362,  3426,  3491,  3555,
    3619,  3684,  3748,  3812,  3876,  3941,  4005,  4069,  4149,  4230,  4310,  4390,  4470,
    4550,  4631,  4711,  4791,  4871,  4967,  5064,  5160,  5256,  5352,  5448,  5544,  5641,
    5737,  5849,  5961,  6073,  6185,  6297,  6410,  6522,  6650,  6778,  6906,  7034,  7162,
    7290,  7435,  7579,  7723,  7867,  8011,  8155,  8315,  8475,  8635,  8795,  8956,  9132,
    9308,  9484,  9660,  9836,  10028, 10220, 10412, 10604, 10812, 11020, 11228, 11437, 11661,
    11885, 12109, 12333, 12573, 12813, 13053, 13309, 13565, 13821, 14093, 14365, 14637, 14925,
    15213, 15502, 15806, 16110, 16414, 16734, 17054, 17390, 17726, 18062, 18414, 18766, 19134,
    19502, 19886, 20270, 20670, 21070, 21486, 21902, 22334, 22766, 23214, 23662, 24126, 24590,
    25070, 25551, 26047, 26559, 27071, 27599, 28143, 28687, 29247,
};
int16_t eb_av1_dc_quant_q3(int32_t qindex, int32_t delta, AomBitDepth bit_depth) {
    switch (bit_depth) {
    case AOM_BITS_8: return dc_qlookup_q3[clamp(qindex + delta, 0, MAXQ)];
    case AOM_BITS_10: return dc_qlookup_10_q3[clamp(qindex + delta, 0, MAXQ)];
    case AOM_BITS_12: return dc_qlookup_12_q3[clamp(qindex + delta, 0, MAXQ)];
    default: assert(0 && "bit_depth should be AOM_BITS_8, AOM_BITS_10 or AOM_BITS_12"); return -1;
    }
}
int16_t eb_av1_ac_quant_q3(int32_t qindex, int32_t delta, AomBitDepth bit_depth) {
    switch (bit_depth) {
    case AOM_BITS_8: return ac_qlookup_q3[clamp(qindex + delta, 0, MAXQ)];
    case AOM_BITS_10: return ac_qlookup_10_q3[clamp(qindex + delta, 0, MAXQ)];
    case AOM_BITS_12: return ac_qlookup_12_q3[clamp(qindex + delta, 0, MAXQ)];
    default: assert(0 && "bit_depth should be AOM_BITS_8, AOM_BITS_10 or AOM_BITS_12"); return -1;
    }
}

static int32_t get_qzbin_factor(int32_t q, AomBitDepth bit_depth) {
    const int32_t quant = eb_av1_dc_quant_q3(q, 0, bit_depth);
    switch (bit_depth) {
    case AOM_BITS_8: return q == 0 ? 64 : (quant < 148 ? 84 : 80);
    case AOM_BITS_10: return q == 0 ? 64 : (quant < 592 ? 84 : 80);
    case AOM_BITS_12: return q == 0 ? 64 : (quant < 2368 ? 84 : 80);
    default: assert(0 && "bit_depth should be AOM_BITS_8, AOM_BITS_10 or AOM_BITS_12"); return -1;
    }
}

// In AV1 TX, the coefficients are always scaled up a factor of 8 (3
// bits), so qtx == Q3.

int16_t eb_av1_dc_quant_qtx(int32_t qindex, int32_t delta, AomBitDepth bit_depth) {
    return eb_av1_dc_quant_q3(qindex, delta, bit_depth);
}
int16_t eb_av1_ac_quant_qtx(int32_t qindex, int32_t delta, AomBitDepth bit_depth) {
    return eb_av1_ac_quant_q3(qindex, delta, bit_depth);
}

static void invert_quant(int16_t *quant, int16_t *shift, int32_t d) {
    uint32_t t;
    int32_t  l, m;
    t = d;
    for (l = 0; t > 1; l++) t >>= 1;
    m      = 1 + (1 << (16 + l)) / d;
    *quant = (int16_t)(m - (1 << 16));
    *shift = 1 << (16 - l);
}

static INLINE int32_t aom_get_qmlevel(int32_t qindex, int32_t first, int32_t last) {
    return first + (qindex * (last + 1 - first)) / QINDEX_RANGE;
}

void set_global_motion_field(PictureControlSet *pcs_ptr) {
    // Init Global Motion Vector
    uint8_t frame_index;
    for (frame_index = INTRA_FRAME; frame_index <= ALTREF_FRAME; ++frame_index) {
        pcs_ptr->parent_pcs_ptr->global_motion[frame_index].wmtype   = IDENTITY;
        pcs_ptr->parent_pcs_ptr->global_motion[frame_index].alpha    = 0;
        pcs_ptr->parent_pcs_ptr->global_motion[frame_index].beta     = 0;
        pcs_ptr->parent_pcs_ptr->global_motion[frame_index].delta    = 0;
        pcs_ptr->parent_pcs_ptr->global_motion[frame_index].gamma    = 0;
        pcs_ptr->parent_pcs_ptr->global_motion[frame_index].invalid  = 0;
        pcs_ptr->parent_pcs_ptr->global_motion[frame_index].wmmat[0] = 0;
        pcs_ptr->parent_pcs_ptr->global_motion[frame_index].wmmat[1] = 0;
        pcs_ptr->parent_pcs_ptr->global_motion[frame_index].wmmat[2] = (1 << WARPEDMODEL_PREC_BITS);
        pcs_ptr->parent_pcs_ptr->global_motion[frame_index].wmmat[3] = 0;
        pcs_ptr->parent_pcs_ptr->global_motion[frame_index].wmmat[4] = 0;
        pcs_ptr->parent_pcs_ptr->global_motion[frame_index].wmmat[5] = (1 << WARPEDMODEL_PREC_BITS);
        pcs_ptr->parent_pcs_ptr->global_motion[frame_index].wmmat[6] = 0;
        pcs_ptr->parent_pcs_ptr->global_motion[frame_index].wmmat[7] = 0;
    }

    //Update MV
#if GLOBAL_WARPED_MOTION
    PictureParentControlSet *parent_pcs_ptr = pcs_ptr->parent_pcs_ptr;
#if GLOBAL_WARPED_MOTION
    if (parent_pcs_ptr->gm_level <= GM_DOWN) {
#endif
        if (parent_pcs_ptr
                ->is_global_motion[get_list_idx(LAST_FRAME)][get_ref_frame_idx(LAST_FRAME)])
            parent_pcs_ptr->global_motion[LAST_FRAME] =
                parent_pcs_ptr->global_motion_estimation[get_list_idx(LAST_FRAME)]
                                                        [get_ref_frame_idx(LAST_FRAME)];
        if (parent_pcs_ptr
                ->is_global_motion[get_list_idx(BWDREF_FRAME)][get_ref_frame_idx(BWDREF_FRAME)])
            parent_pcs_ptr->global_motion[BWDREF_FRAME] =
                parent_pcs_ptr->global_motion_estimation[get_list_idx(BWDREF_FRAME)]
                                                        [get_ref_frame_idx(BWDREF_FRAME)];
#if GLOBAL_WARPED_MOTION
        // Upscale the translation parameters by 2, because the search is done on a down-sampled
        // version of the source picture (with a down-sampling factor of 2 in each dimension).
        if (parent_pcs_ptr->gm_level == GM_DOWN) {
            parent_pcs_ptr->global_motion[LAST_FRAME].wmmat[0] *= 2;
            parent_pcs_ptr->global_motion[LAST_FRAME].wmmat[1] *= 2;
            parent_pcs_ptr->global_motion[BWDREF_FRAME].wmmat[0] *= 2;
            parent_pcs_ptr->global_motion[BWDREF_FRAME].wmmat[1] *= 2;
        }
#endif
#endif
#if GLOBAL_WARPED_MOTION
    } else {
        if (pcs_ptr->parent_pcs_ptr->is_pan && pcs_ptr->parent_pcs_ptr->is_tilt) {
            pcs_ptr->parent_pcs_ptr->global_motion[LAST_FRAME].wmtype = TRANSLATION;
            pcs_ptr->parent_pcs_ptr->global_motion[LAST_FRAME].wmmat[1] =
                ((pcs_ptr->parent_pcs_ptr->pan_mvx + pcs_ptr->parent_pcs_ptr->tilt_mvx) / 2)
                << 1 << GM_TRANS_ONLY_PREC_DIFF;
            pcs_ptr->parent_pcs_ptr->global_motion[LAST_FRAME].wmmat[0] =
                ((pcs_ptr->parent_pcs_ptr->pan_mvy + pcs_ptr->parent_pcs_ptr->tilt_mvy) / 2)
                << 1 << GM_TRANS_ONLY_PREC_DIFF;
        } else if (pcs_ptr->parent_pcs_ptr->is_pan) {
            pcs_ptr->parent_pcs_ptr->global_motion[LAST_FRAME].wmtype = TRANSLATION;
            pcs_ptr->parent_pcs_ptr->global_motion[LAST_FRAME].wmmat[1] =
                pcs_ptr->parent_pcs_ptr->pan_mvx << 1 << GM_TRANS_ONLY_PREC_DIFF;
            pcs_ptr->parent_pcs_ptr->global_motion[LAST_FRAME].wmmat[0] =
                pcs_ptr->parent_pcs_ptr->pan_mvy << 1 << GM_TRANS_ONLY_PREC_DIFF;
        } else if (pcs_ptr->parent_pcs_ptr->is_tilt) {
            pcs_ptr->parent_pcs_ptr->global_motion[LAST_FRAME].wmtype = TRANSLATION;
            pcs_ptr->parent_pcs_ptr->global_motion[LAST_FRAME].wmmat[1] =
                pcs_ptr->parent_pcs_ptr->tilt_mvx << 1 << GM_TRANS_ONLY_PREC_DIFF;
            pcs_ptr->parent_pcs_ptr->global_motion[LAST_FRAME].wmmat[0] =
                pcs_ptr->parent_pcs_ptr->tilt_mvy << 1 << GM_TRANS_ONLY_PREC_DIFF;
        }

        pcs_ptr->parent_pcs_ptr->global_motion[LAST_FRAME].wmmat[1] =
            (int32_t)clamp(pcs_ptr->parent_pcs_ptr->global_motion[LAST_FRAME].wmmat[1],
                           GM_TRANS_MIN * GM_TRANS_DECODE_FACTOR,
                           GM_TRANS_MAX * GM_TRANS_DECODE_FACTOR);
        pcs_ptr->parent_pcs_ptr->global_motion[LAST_FRAME].wmmat[0] =
            (int32_t)clamp(pcs_ptr->parent_pcs_ptr->global_motion[LAST_FRAME].wmmat[0],
                           GM_TRANS_MIN * GM_TRANS_DECODE_FACTOR,
                           GM_TRANS_MAX * GM_TRANS_DECODE_FACTOR);

        pcs_ptr->parent_pcs_ptr->global_motion[BWDREF_FRAME].wmtype = TRANSLATION;
        pcs_ptr->parent_pcs_ptr->global_motion[BWDREF_FRAME].wmmat[1] =
            0 - pcs_ptr->parent_pcs_ptr->global_motion[LAST_FRAME].wmmat[1];
        pcs_ptr->parent_pcs_ptr->global_motion[BWDREF_FRAME].wmmat[0] =
            0 - pcs_ptr->parent_pcs_ptr->global_motion[LAST_FRAME].wmmat[0];
        pcs_ptr->parent_pcs_ptr->global_motion[BWDREF_FRAME].wmmat[1] =
            (int32_t)clamp(pcs_ptr->parent_pcs_ptr->global_motion[BWDREF_FRAME].wmmat[1],
                           GM_TRANS_MIN * GM_TRANS_DECODE_FACTOR,
                           GM_TRANS_MAX * GM_TRANS_DECODE_FACTOR);
        pcs_ptr->parent_pcs_ptr->global_motion[BWDREF_FRAME].wmmat[0] =
            (int32_t)clamp(pcs_ptr->parent_pcs_ptr->global_motion[BWDREF_FRAME].wmmat[0],
                           GM_TRANS_MIN * GM_TRANS_DECODE_FACTOR,
                           GM_TRANS_MAX * GM_TRANS_DECODE_FACTOR);
    }
#endif
}

void eb_av1_set_quantizer(PictureParentControlSet *pcs_ptr, int32_t q) {
    // quantizer has to be reinitialized with av1_init_quantizer() if any
    // delta_q changes.
    FrameHeader *frm_hdr = &pcs_ptr->frm_hdr;

    frm_hdr->quantization_params.using_qmatrix = 0;
    pcs_ptr->min_qmlevel                       = 5;
    pcs_ptr->max_qmlevel                       = 9;

    frm_hdr->quantization_params.base_q_idx = AOMMAX(frm_hdr->delta_q_params.delta_q_present, q);
    frm_hdr->quantization_params.delta_q_dc[AOM_PLANE_Y] = 0;
    frm_hdr->quantization_params.delta_q_ac[AOM_PLANE_Y] = 0;
    frm_hdr->quantization_params.delta_q_ac[AOM_PLANE_U] = 0;
    frm_hdr->quantization_params.delta_q_dc[AOM_PLANE_U] = 0;
    frm_hdr->quantization_params.delta_q_ac[AOM_PLANE_V] = 0;
    frm_hdr->quantization_params.delta_q_dc[AOM_PLANE_V] = 0;
    frm_hdr->quantization_params.qm[AOM_PLANE_Y]         = aom_get_qmlevel(
        frm_hdr->quantization_params.base_q_idx, pcs_ptr->min_qmlevel, pcs_ptr->max_qmlevel);
    frm_hdr->quantization_params.qm[AOM_PLANE_U] =
        aom_get_qmlevel(frm_hdr->quantization_params.base_q_idx +
                            frm_hdr->quantization_params.delta_q_ac[AOM_PLANE_U],
                        pcs_ptr->min_qmlevel,
                        pcs_ptr->max_qmlevel);

    if (!pcs_ptr->separate_uv_delta_q)
        frm_hdr->quantization_params.qm[AOM_PLANE_V] = frm_hdr->quantization_params.qm[AOM_PLANE_U];
    else
        frm_hdr->quantization_params.qm[AOM_PLANE_V] =
            aom_get_qmlevel(frm_hdr->quantization_params.base_q_idx +
                                frm_hdr->quantization_params.delta_q_ac[AOM_PLANE_V],
                            pcs_ptr->min_qmlevel,
                            pcs_ptr->max_qmlevel);
}

void eb_av1_build_quantizer(AomBitDepth bit_depth, int32_t y_dc_delta_q, int32_t u_dc_delta_q,
                            int32_t u_ac_delta_q, int32_t v_dc_delta_q, int32_t v_ac_delta_q,
                            Quants *const quants, Dequants *const deq) {
    int32_t i, q, quant_q3, quant_qtx;

    for (q = 0; q < QINDEX_RANGE; q++) {
        const int32_t qzbin_factor     = get_qzbin_factor(q, bit_depth);
        const int32_t qrounding_factor = q == 0 ? 64 : 48;

        for (i = 0; i < 2; ++i) {
            int32_t qrounding_factor_fp = 64;
            // y quantizer setup with original coeff shift of Q3
            quant_q3 = i == 0 ? eb_av1_dc_quant_q3(q, y_dc_delta_q, bit_depth)
                              : eb_av1_ac_quant_q3(q, 0, bit_depth);
            // y quantizer with TX scale
            quant_qtx = i == 0 ? eb_av1_dc_quant_qtx(q, y_dc_delta_q, bit_depth)
                               : eb_av1_ac_quant_qtx(q, 0, bit_depth);
            invert_quant(&quants->y_quant[q][i], &quants->y_quant_shift[q][i], quant_qtx);
            quants->y_quant_fp[q][i] = (int16_t)((1 << 16) / quant_qtx);
            quants->y_round_fp[q][i] = (int16_t)((qrounding_factor_fp * quant_qtx) >> 7);
            quants->y_zbin[q][i]     = (int16_t)ROUND_POWER_OF_TWO(qzbin_factor * quant_qtx, 7);
            quants->y_round[q][i]    = (int16_t)((qrounding_factor * quant_qtx) >> 7);
            deq->y_dequant_qtx[q][i] = (int16_t)quant_qtx;
            deq->y_dequant_q3[q][i]  = (int16_t)quant_q3;

            // u quantizer setup with original coeff shift of Q3
            quant_q3 = i == 0 ? eb_av1_dc_quant_q3(q, u_dc_delta_q, bit_depth)
                              : eb_av1_ac_quant_q3(q, u_ac_delta_q, bit_depth);
            // u quantizer with TX scale
            quant_qtx = i == 0 ? eb_av1_dc_quant_qtx(q, u_dc_delta_q, bit_depth)
                               : eb_av1_ac_quant_qtx(q, u_ac_delta_q, bit_depth);
            invert_quant(&quants->u_quant[q][i], &quants->u_quant_shift[q][i], quant_qtx);
            quants->u_quant_fp[q][i] = (int16_t)((1 << 16) / quant_qtx);
            quants->u_round_fp[q][i] = (int16_t)((qrounding_factor_fp * quant_qtx) >> 7);
            quants->u_zbin[q][i]     = (int16_t)ROUND_POWER_OF_TWO(qzbin_factor * quant_qtx, 7);
            quants->u_round[q][i]    = (int16_t)((qrounding_factor * quant_qtx) >> 7);
            deq->u_dequant_qtx[q][i] = (int16_t)quant_qtx;
            deq->u_dequant_q3[q][i]  = (int16_t)quant_q3;

            // v quantizer setup with original coeff shift of Q3
            quant_q3 = i == 0 ? eb_av1_dc_quant_q3(q, v_dc_delta_q, bit_depth)
                              : eb_av1_ac_quant_q3(q, v_ac_delta_q, bit_depth);
            // v quantizer with TX scale
            quant_qtx = i == 0 ? eb_av1_dc_quant_qtx(q, v_dc_delta_q, bit_depth)
                               : eb_av1_ac_quant_qtx(q, v_ac_delta_q, bit_depth);
            invert_quant(&quants->v_quant[q][i], &quants->v_quant_shift[q][i], quant_qtx);
            quants->v_quant_fp[q][i] = (int16_t)((1 << 16) / quant_qtx);
            quants->v_round_fp[q][i] = (int16_t)((qrounding_factor_fp * quant_qtx) >> 7);
            quants->v_zbin[q][i]     = (int16_t)ROUND_POWER_OF_TWO(qzbin_factor * quant_qtx, 7);
            quants->v_round[q][i]    = (int16_t)((qrounding_factor * quant_qtx) >> 7);
            deq->v_dequant_qtx[q][i] = (int16_t)quant_qtx;
            deq->v_dequant_q3[q][i]  = (int16_t)quant_q3;
        }

        for (i = 2; i < 8; i++) { // 8: SIMD width
            quants->y_quant[q][i]       = quants->y_quant[q][1];
            quants->y_quant_fp[q][i]    = quants->y_quant_fp[q][1];
            quants->y_round_fp[q][i]    = quants->y_round_fp[q][1];
            quants->y_quant_shift[q][i] = quants->y_quant_shift[q][1];
            quants->y_zbin[q][i]        = quants->y_zbin[q][1];
            quants->y_round[q][i]       = quants->y_round[q][1];
            deq->y_dequant_qtx[q][i]    = deq->y_dequant_qtx[q][1];
            deq->y_dequant_q3[q][i]     = deq->y_dequant_q3[q][1];

            quants->u_quant[q][i]       = quants->u_quant[q][1];
            quants->u_quant_fp[q][i]    = quants->u_quant_fp[q][1];
            quants->u_round_fp[q][i]    = quants->u_round_fp[q][1];
            quants->u_quant_shift[q][i] = quants->u_quant_shift[q][1];
            quants->u_zbin[q][i]        = quants->u_zbin[q][1];
            quants->u_round[q][i]       = quants->u_round[q][1];
            deq->u_dequant_qtx[q][i]    = deq->u_dequant_qtx[q][1];
            deq->u_dequant_q3[q][i]     = deq->u_dequant_q3[q][1];
            quants->v_quant[q][i]       = quants->u_quant[q][1];
            quants->v_quant_fp[q][i]    = quants->v_quant_fp[q][1];
            quants->v_round_fp[q][i]    = quants->v_round_fp[q][1];
            quants->v_quant_shift[q][i] = quants->v_quant_shift[q][1];
            quants->v_zbin[q][i]        = quants->v_zbin[q][1];
            quants->v_round[q][i]       = quants->v_round[q][1];
            deq->v_dequant_qtx[q][i]    = deq->v_dequant_qtx[q][1];
            deq->v_dequant_q3[q][i]     = deq->v_dequant_q3[q][1];
        }
    }
}

void eb_av1_qm_init(PictureParentControlSet *pcs_ptr) {
    const uint8_t num_planes = 3; // MAX_MB_PLANE;// NM- No monochroma
    uint8_t       q, c, t;
    int32_t       current;
    for (q = 0; q < NUM_QM_LEVELS; ++q) {
        for (c = 0; c < num_planes; ++c) {
            current = 0;
            for (t = 0; t < TX_SIZES_ALL; ++t) {
                const int32_t size       = tx_size_2d[t];
                const TxSize  qm_tx_size = av1_get_adjusted_tx_size(t);
                if (q == NUM_QM_LEVELS - 1) {
                    pcs_ptr->gqmatrix[q][c][t]  = NULL;
                    pcs_ptr->giqmatrix[q][c][t] = NULL;
                } else if (t != qm_tx_size) { // Reuse matrices for 'qm_tx_size'
                    pcs_ptr->gqmatrix[q][c][t]  = pcs_ptr->gqmatrix[q][c][qm_tx_size];
                    pcs_ptr->giqmatrix[q][c][t] = pcs_ptr->giqmatrix[q][c][qm_tx_size];
                } else {
                    assert(current + size <= QM_TOTAL_SIZE);
                    pcs_ptr->gqmatrix[q][c][t]  = &wt_matrix_ref[q][c >= 1][current];
                    pcs_ptr->giqmatrix[q][c][t] = &iwt_matrix_ref[q][c >= 1][current];
                    current += size;
                }
            }
        }
    }
}

/******************************************************
* Compute picture and slice level chroma QP offsets
******************************************************/
void set_slice_and_picture_chroma_qp_offsets(PictureControlSet *pcs_ptr) {
    // This is a picture level chroma QP offset and is sent in the PPS
    pcs_ptr->cb_qp_offset = 0;
    pcs_ptr->cr_qp_offset = 0;

    //In order to have QP offsets for chroma at a slice level set slice_level_chroma_qp_flag flag in pcs_ptr (can be done in the PCS Ctor)

    // The below are slice level chroma QP offsets and is sent for each slice when slice_level_chroma_qp_flag is set

    // IMPORTANT: Lambda tables assume that the cb and cr have the same QP offsets.
    // However the offsets for each component can be very different for ENC DEC and we are conformant.
    pcs_ptr->slice_cb_qp_offset = 0;
    pcs_ptr->slice_cr_qp_offset = 0;

    if (pcs_ptr->parent_pcs_ptr->pic_noise_class >= PIC_NOISE_CLASS_6) {
        pcs_ptr->slice_cb_qp_offset = 10;
        pcs_ptr->slice_cr_qp_offset = 10;
    } else if (pcs_ptr->parent_pcs_ptr->pic_noise_class >= PIC_NOISE_CLASS_4) {
        pcs_ptr->slice_cb_qp_offset = 8;
        pcs_ptr->slice_cr_qp_offset = 8;
    } else {
        if (pcs_ptr->temporal_layer_index == 1) {
            pcs_ptr->slice_cb_qp_offset = 2;
            pcs_ptr->slice_cr_qp_offset = 2;
        } else {
            pcs_ptr->slice_cb_qp_offset = 0;
            pcs_ptr->slice_cr_qp_offset = 0;
        }
    }
}

/******************************************************
* Set the reference sg ep for a given picture
******************************************************/
void set_reference_sg_ep(PictureControlSet *pcs_ptr) {
    Av1Common *        cm = pcs_ptr->parent_pcs_ptr->av1_cm;
    EbReferenceObject *ref_obj_l0, *ref_obj_l1;
    memset(cm->sg_frame_ep_cnt, 0, SGRPROJ_PARAMS * sizeof(int32_t));
    cm->sg_frame_ep = 0;

    // NADER: set cm->sg_ref_frame_ep[0] = cm->sg_ref_frame_ep[1] = -1 to perform all iterations
    switch (pcs_ptr->slice_type) {
    case I_SLICE:
        cm->sg_ref_frame_ep[0] = -1;
        cm->sg_ref_frame_ep[1] = -1;
        break;
    case B_SLICE:
        ref_obj_l0 = (EbReferenceObject *)pcs_ptr->ref_pic_ptr_array[REF_LIST_0][0]->object_ptr;
        ref_obj_l1 = (EbReferenceObject *)pcs_ptr->ref_pic_ptr_array[REF_LIST_1][0]->object_ptr;
        cm->sg_ref_frame_ep[0] = ref_obj_l0->sg_frame_ep;
        cm->sg_ref_frame_ep[1] = ref_obj_l1->sg_frame_ep;
        break;
    case P_SLICE:
        ref_obj_l0 = (EbReferenceObject *)pcs_ptr->ref_pic_ptr_array[REF_LIST_0][0]->object_ptr;
        cm->sg_ref_frame_ep[0] = ref_obj_l0->sg_frame_ep;
        cm->sg_ref_frame_ep[1] = 0;
        break;
    default: SVT_LOG("SG: Not supported picture type"); break;
    }
}

/******************************************************
* Set the reference cdef strength for a given picture
******************************************************/
void set_reference_cdef_strength(PictureControlSet *pcs_ptr) {
    EbReferenceObject *ref_obj_l0, *ref_obj_l1;
    int32_t            strength;
    // NADER: set pcs_ptr->parent_pcs_ptr->use_ref_frame_cdef_strength 0 to test all strengths
    switch (pcs_ptr->slice_type) {
    case I_SLICE:
        pcs_ptr->parent_pcs_ptr->use_ref_frame_cdef_strength = 0;
        pcs_ptr->parent_pcs_ptr->cdf_ref_frame_strenght      = 0;
        break;
    case B_SLICE:
        ref_obj_l0 = (EbReferenceObject *)pcs_ptr->ref_pic_ptr_array[REF_LIST_0][0]->object_ptr;
        ref_obj_l1 = (EbReferenceObject *)pcs_ptr->ref_pic_ptr_array[REF_LIST_1][0]->object_ptr;
        strength   = (ref_obj_l0->cdef_frame_strength + ref_obj_l1->cdef_frame_strength) / 2;
        pcs_ptr->parent_pcs_ptr->use_ref_frame_cdef_strength = 1;
        pcs_ptr->parent_pcs_ptr->cdf_ref_frame_strenght      = strength;
        break;
    case P_SLICE:
        ref_obj_l0 = (EbReferenceObject *)pcs_ptr->ref_pic_ptr_array[REF_LIST_0][0]->object_ptr;
        strength   = ref_obj_l0->cdef_frame_strength;
        pcs_ptr->parent_pcs_ptr->use_ref_frame_cdef_strength = 1;
        pcs_ptr->parent_pcs_ptr->cdf_ref_frame_strenght      = strength;
        break;
    default: SVT_LOG("CDEF: Not supported picture type"); break;
    }
}

/******************************************************
* Compute Tc, and Beta offsets for a given picture
******************************************************/

static void mode_decision_configuration_context_dctor(EbPtr p) {
    EbThreadContext *                 thread_context_ptr = (EbThreadContext *)p;
    ModeDecisionConfigurationContext *obj =
        (ModeDecisionConfigurationContext *)thread_context_ptr->priv;

    if (obj->is_md_rate_estimation_ptr_owner) EB_FREE_ARRAY(obj->md_rate_estimation_ptr);
    EB_FREE_ARRAY(obj->sb_score_array);
    EB_FREE_ARRAY(obj->sb_cost_array);
    EB_FREE_ARRAY(obj->mdc_candidate_ptr);
    EB_FREE_ARRAY(obj->mdc_ref_mv_stack);
    EB_FREE_ARRAY(obj->mdc_blk_ptr->av1xd);
    EB_FREE_ARRAY(obj->mdc_blk_ptr);
    EB_FREE_ARRAY(obj);
}
/******************************************************
 * Mode Decision Configuration Context Constructor
 ******************************************************/
EbErrorType mode_decision_configuration_context_ctor(EbThreadContext *  thread_context_ptr,
                                                     const EbEncHandle *enc_handle_ptr,
                                                     int input_index, int output_index) {
    const SequenceControlSet *scs_ptr = enc_handle_ptr->scs_instance_array[0]->scs_ptr;
    uint32_t                  sb_total_count =
        ((scs_ptr->max_input_luma_width + BLOCK_SIZE_64 - 1) / BLOCK_SIZE_64) *
        ((scs_ptr->max_input_luma_height + BLOCK_SIZE_64 - 1) / BLOCK_SIZE_64);

    ModeDecisionConfigurationContext *context_ptr;
    EB_CALLOC_ARRAY(context_ptr, 1);
    thread_context_ptr->priv  = context_ptr;
    thread_context_ptr->dctor = mode_decision_configuration_context_dctor;

    // Input/Output System Resource Manager FIFOs
    context_ptr->rate_control_input_fifo_ptr = eb_system_resource_get_consumer_fifo(
        enc_handle_ptr->rate_control_results_resource_ptr, input_index);
    context_ptr->mode_decision_configuration_output_fifo_ptr = eb_system_resource_get_producer_fifo(
        enc_handle_ptr->enc_dec_tasks_resource_ptr, output_index);
    // Rate estimation
    EB_MALLOC_ARRAY(context_ptr->md_rate_estimation_ptr, 1);
    context_ptr->is_md_rate_estimation_ptr_owner = EB_TRUE;

    // Adaptive Depth Partitioning
    EB_MALLOC_ARRAY(context_ptr->sb_score_array, sb_total_count);
    EB_MALLOC_ARRAY(context_ptr->sb_cost_array, sb_total_count);

    // Open Loop Partitioning
    EB_MALLOC_ARRAY(context_ptr->mdc_candidate_ptr, 1);
    EB_MALLOC_ARRAY(context_ptr->mdc_ref_mv_stack, 1);
    EB_MALLOC_ARRAY(context_ptr->mdc_blk_ptr, 1);
    context_ptr->mdc_blk_ptr->av1xd = NULL;
    EB_MALLOC_ARRAY(context_ptr->mdc_blk_ptr->av1xd, 1);
    return EB_ErrorNone;
}

/******************************************************
* Predict the SB partitionning
******************************************************/
void perform_early_sb_partitionning(ModeDecisionConfigurationContext *context_ptr,
                                    SequenceControlSet *scs_ptr, PictureControlSet *pcs_ptr) {
    SuperBlock *sb_ptr;
    uint32_t    sb_index;
    pcs_ptr->parent_pcs_ptr->average_qp = (uint8_t)pcs_ptr->parent_pcs_ptr->picture_qp;

    // SB Loop : Partitionnig Decision
    for (sb_index = 0; sb_index < pcs_ptr->sb_total_count; ++sb_index) {
        sb_ptr = pcs_ptr->sb_ptr_array[sb_index];
        early_mode_decision_sb(scs_ptr, pcs_ptr, sb_ptr, sb_index, context_ptr);
    } // End of SB Loop
}

void perform_early_sb_partitionning_sb(ModeDecisionConfigurationContext *context_ptr,
                                       SequenceControlSet *scs_ptr, PictureControlSet *pcs_ptr,
                                       uint32_t sb_index) {
    SuperBlock *sb_ptr;

    // SB Loop : Partitionnig Decision
    sb_ptr = pcs_ptr->sb_ptr_array[sb_index];
    early_mode_decision_sb(scs_ptr, pcs_ptr, sb_ptr, sb_index, context_ptr);
}

void forward_all_blocks_to_md(SequenceControlSet *scs_ptr, PictureControlSet *pcs_ptr) {
    uint32_t sb_index;
    EbBool   split_flag;

    UNUSED(split_flag);

    for (sb_index = 0; sb_index < pcs_ptr->sb_total_count_pix; ++sb_index) {
        MdcSbData *results_ptr = &pcs_ptr->mdc_sb_array[sb_index];

        results_ptr->leaf_count = 0;

        uint32_t blk_index = 0;

        while (blk_index < scs_ptr->max_block_cnt) {
            split_flag = EB_TRUE;

            const BlockGeom *blk_geom = get_blk_geom_mds(blk_index);

            //if the parentSq is inside inject this block
            uint8_t is_blk_allowed =
                pcs_ptr->slice_type != I_SLICE ? 1 : (blk_geom->sq_size < 128) ? 1 : 0;

            if (pcs_ptr->parent_pcs_ptr->sb_geom[sb_index].block_is_inside_md_scan[blk_index] && is_blk_allowed)

            {
                results_ptr->leaf_data_array[results_ptr->leaf_count].tot_d1_blocks =

                    blk_geom->sq_size == 128
                        ? 17
                        : blk_geom->sq_size > 8 ? 25 : blk_geom->sq_size == 8 ? 5 : 1;

                results_ptr->leaf_data_array[results_ptr->leaf_count].leaf_index =
                    0; //valid only for square 85 world. will be removed.
                results_ptr->leaf_data_array[results_ptr->leaf_count].mds_idx = blk_index;
                if (blk_geom->sq_size > 4) {
                    results_ptr->leaf_data_array[results_ptr->leaf_count++].split_flag = EB_TRUE;
                    split_flag                                                         = EB_TRUE;
                } else {
                    results_ptr->leaf_data_array[results_ptr->leaf_count++].split_flag = EB_FALSE;
                    split_flag                                                         = EB_FALSE;
                }
            }

            blk_index++;
        }
    }

    pcs_ptr->parent_pcs_ptr->average_qp = (uint8_t)pcs_ptr->parent_pcs_ptr->picture_qp;
}

void forward_sq_blocks_to_md(SequenceControlSet *scs_ptr, PictureControlSet *pcs_ptr) {
    uint32_t sb_index;
    EbBool   split_flag;

    for (sb_index = 0; sb_index < pcs_ptr->sb_total_count_pix; ++sb_index) {
        MdcSbData *results_ptr = &pcs_ptr->mdc_sb_array[sb_index];

        results_ptr->leaf_count = 0;

        uint32_t blk_index =
            pcs_ptr->slice_type == I_SLICE && scs_ptr->seq_header.sb_size == BLOCK_128X128 ? 17 : 0;

        while (blk_index < scs_ptr->max_block_cnt) {
            split_flag = EB_TRUE;

            const BlockGeom *blk_geom = get_blk_geom_mds(blk_index);

            //if the parentSq is inside inject this block
            if (pcs_ptr->parent_pcs_ptr->sb_geom[sb_index].block_is_inside_md_scan[blk_index])

            {
                //int32_t offset_d1 = ns_blk_offset[(int32_t)from_shape_to_part[blk_geom->shape]]; //blk_ptr->best_d1_blk; // TOCKECK
                //int32_t num_d1_block = ns_blk_num[(int32_t)from_shape_to_part[blk_geom->shape]]; // context_ptr->blk_geom->totns; // TOCKECK
                //
                //                                                  // for (int32_t d1_itr = blk_it; d1_itr < blk_it + num_d1_block; d1_itr++) {
                // for (int32_t d1_itr = (int32_t)blk_index ; d1_itr < (int32_t)blk_index +  num_d1_block ; d1_itr++) {
                results_ptr->leaf_data_array[results_ptr->leaf_count].tot_d1_blocks = 1;

                results_ptr->leaf_data_array[results_ptr->leaf_count].leaf_index =
                    0; //valid only for square 85 world. will be removed.
                results_ptr->leaf_data_array[results_ptr->leaf_count].mds_idx = blk_index;

                if (blk_geom->sq_size > 4) {
                    results_ptr->leaf_data_array[results_ptr->leaf_count++].split_flag = EB_TRUE;
                    split_flag                                                         = EB_TRUE;
                } else {
                    results_ptr->leaf_data_array[results_ptr->leaf_count++].split_flag = EB_FALSE;
                    split_flag                                                         = EB_FALSE;
                }
            }
            blk_index +=
                split_flag
                    ? d1_depth_offset[scs_ptr->seq_header.sb_size == BLOCK_128X128][blk_geom->depth]
                    : ns_depth_offset[scs_ptr->seq_header.sb_size == BLOCK_128X128]
                                     [blk_geom->depth];
        }
    }

    pcs_ptr->parent_pcs_ptr->average_qp = (uint8_t)pcs_ptr->parent_pcs_ptr->picture_qp;
}

void sb_forward_sq_blocks_to_md(SequenceControlSet *scs_ptr, PictureControlSet *pcs_ptr,
                                uint32_t sb_index) {
    EbBool     split_flag;
    MdcSbData *results_ptr  = &pcs_ptr->mdc_sb_array[sb_index];
    results_ptr->leaf_count = 0;
    uint32_t blk_index =
        pcs_ptr->slice_type == I_SLICE && scs_ptr->seq_header.sb_size == BLOCK_128X128 ? 17 : 0;

    while (blk_index < scs_ptr->max_block_cnt) {
        split_flag = EB_TRUE;

        const BlockGeom *blk_geom = get_blk_geom_mds(blk_index);

        if (pcs_ptr->parent_pcs_ptr->sb_geom[sb_index].block_is_inside_md_scan[blk_index]) {
            results_ptr->leaf_data_array[results_ptr->leaf_count].tot_d1_blocks = 1;
            results_ptr->leaf_data_array[results_ptr->leaf_count].leaf_index =
                0; //valid only for square 85 world. will be removed.
            results_ptr->leaf_data_array[results_ptr->leaf_count].mds_idx = blk_index;

            if (blk_geom->sq_size > 4) {
                results_ptr->leaf_data_array[results_ptr->leaf_count++].split_flag = EB_TRUE;
                split_flag                                                         = EB_TRUE;
            } else {
                results_ptr->leaf_data_array[results_ptr->leaf_count++].split_flag = EB_FALSE;
                split_flag                                                         = EB_FALSE;
            }
        }
        blk_index +=
            split_flag
                ? d1_depth_offset[scs_ptr->seq_header.sb_size == BLOCK_128X128][blk_geom->depth]
                : ns_depth_offset[scs_ptr->seq_header.sb_size == BLOCK_128X128][blk_geom->depth];
    }
    pcs_ptr->parent_pcs_ptr->average_qp = (uint8_t)pcs_ptr->parent_pcs_ptr->picture_qp;
}

/******************************************************
* Derive MD parameters
******************************************************/
void set_md_settings(SequenceControlSet *scs_ptr, PictureControlSet *pcs_ptr) {
    // Initialize the homogeneous area threshold
    // Set the MD Open Loop Flag
    // HG - to clean up the intra_md_open_loop_flag derivation

    pcs_ptr->intra_md_open_loop_flag = pcs_ptr->temporal_layer_index == 0 ? EB_FALSE : EB_TRUE;

    if (pcs_ptr->parent_pcs_ptr->is_used_as_reference_flag == EB_TRUE &&
        scs_ptr->input_resolution < INPUT_SIZE_4K_RANGE)
        pcs_ptr->intra_md_open_loop_flag = EB_FALSE;

    pcs_ptr->limit_intra             = EB_FALSE;
    pcs_ptr->intra_md_open_loop_flag = EB_FALSE;
}
/******************************************************
* Load the cost of the different partitioning method into a local array and derive sensitive picture flag
    Input   : the offline derived cost per search method, detection signals
    Output  : valid cost_depth_mode and valid sensitivePicture
******************************************************/
void configure_adp(PictureControlSet *pcs_ptr, ModeDecisionConfigurationContext *context_ptr) {
    UNUSED(pcs_ptr);
    context_ptr->cost_depth_mode[SB_SQ_BLOCKS_DEPTH_MODE - 1]      = SQ_BLOCKS_SEARCH_COST;
    context_ptr->cost_depth_mode[SB_SQ_NON4_BLOCKS_DEPTH_MODE - 1] = SQ_NON4_BLOCKS_SEARCH_COST;
    context_ptr->cost_depth_mode[SB_OPEN_LOOP_DEPTH_MODE - 1]      = SB_OPEN_LOOP_COST;
    context_ptr->cost_depth_mode[SB_FAST_OPEN_LOOP_DEPTH_MODE - 1] = SB_FAST_OPEN_LOOP_COST;
    context_ptr->cost_depth_mode[SB_PRED_OPEN_LOOP_DEPTH_MODE - 1] = SB_PRED_OPEN_LOOP_COST;

    // Initialize the score based TH
    context_ptr->score_th[0] = ~0;
    context_ptr->score_th[1] = ~0;
    context_ptr->score_th[2] = ~0;
    context_ptr->score_th[3] = ~0;
    context_ptr->score_th[4] = ~0;
    context_ptr->score_th[5] = ~0;
    context_ptr->score_th[6] = ~0;

    // Initialize the predicted budget
    context_ptr->predicted_cost = (uint32_t)~0;
}

/******************************************************
* Assign a search method based on the allocated cost
    Input   : allocated budget per SB
    Output  : search method per SB
******************************************************/
void derive_search_method(PictureControlSet *               pcs_ptr,
                          ModeDecisionConfigurationContext *context_ptr) {
    uint32_t sb_index;

    for (sb_index = 0; sb_index < pcs_ptr->sb_total_count_pix; sb_index++) {
        if (context_ptr->sb_cost_array[sb_index] ==
            context_ptr->cost_depth_mode[SB_PRED_OPEN_LOOP_DEPTH_MODE - 1])
            pcs_ptr->parent_pcs_ptr->sb_depth_mode_array[sb_index] = SB_PRED_OPEN_LOOP_DEPTH_MODE;
        else if (context_ptr->sb_cost_array[sb_index] ==
                 context_ptr->cost_depth_mode[SB_FAST_OPEN_LOOP_DEPTH_MODE - 1])
            pcs_ptr->parent_pcs_ptr->sb_depth_mode_array[sb_index] = SB_FAST_OPEN_LOOP_DEPTH_MODE;
        else if (context_ptr->sb_cost_array[sb_index] ==
                 context_ptr->cost_depth_mode[SB_OPEN_LOOP_DEPTH_MODE - 1])
            pcs_ptr->parent_pcs_ptr->sb_depth_mode_array[sb_index] = SB_OPEN_LOOP_DEPTH_MODE;
        else if (context_ptr->sb_cost_array[sb_index] ==
                 context_ptr->cost_depth_mode[SB_SQ_NON4_BLOCKS_DEPTH_MODE - 1])
            pcs_ptr->parent_pcs_ptr->sb_depth_mode_array[sb_index] = SB_SQ_NON4_BLOCKS_DEPTH_MODE;
        else
            pcs_ptr->parent_pcs_ptr->sb_depth_mode_array[sb_index] = SB_SQ_BLOCKS_DEPTH_MODE;
    }
}

/******************************************************
* Set SB budget
    Input   : SB score, detection signals, iteration
    Output  : predicted budget for the SB
******************************************************/
void set_sb_budget(SequenceControlSet *scs_ptr, PictureControlSet *pcs_ptr, SuperBlock *sb_ptr,
                   ModeDecisionConfigurationContext *context_ptr) {
    const uint32_t sb_index = sb_ptr->index;
    uint32_t       max_to_min_score, score_to_min;
    UNUSED(scs_ptr);
    UNUSED(pcs_ptr);
    {
        context_ptr->sb_score_array[sb_index] = CLIP3(context_ptr->sb_min_score,
                                                      context_ptr->sb_max_score,
                                                      context_ptr->sb_score_array[sb_index]);
        score_to_min     = context_ptr->sb_score_array[sb_index] - context_ptr->sb_min_score;
        max_to_min_score = context_ptr->sb_max_score - context_ptr->sb_min_score;

        if ((score_to_min <= (max_to_min_score * context_ptr->score_th[0]) / 100 &&
             context_ptr->score_th[0] != 0) ||
            context_ptr->number_of_segments == 1 || context_ptr->score_th[1] == 100) {
            context_ptr->sb_cost_array[sb_index] = context_ptr->interval_cost[0];
            context_ptr->predicted_cost += context_ptr->interval_cost[0];
        } else if ((score_to_min <= (max_to_min_score * context_ptr->score_th[1]) / 100 &&
                    context_ptr->score_th[1] != 0) ||
                   context_ptr->number_of_segments == 2 || context_ptr->score_th[2] == 100) {
            context_ptr->sb_cost_array[sb_index] = context_ptr->interval_cost[1];
            context_ptr->predicted_cost += context_ptr->interval_cost[1];
        } else if ((score_to_min <= (max_to_min_score * context_ptr->score_th[2]) / 100 &&
                    context_ptr->score_th[2] != 0) ||
                   context_ptr->number_of_segments == 3 || context_ptr->score_th[3] == 100) {
            context_ptr->sb_cost_array[sb_index] = context_ptr->interval_cost[2];
            context_ptr->predicted_cost += context_ptr->interval_cost[2];
        } else if ((score_to_min <= (max_to_min_score * context_ptr->score_th[3]) / 100 &&
                    context_ptr->score_th[3] != 0) ||
                   context_ptr->number_of_segments == 4 || context_ptr->score_th[4] == 100) {
            context_ptr->sb_cost_array[sb_index] = context_ptr->interval_cost[3];
            context_ptr->predicted_cost += context_ptr->interval_cost[3];
        } else if ((score_to_min <= (max_to_min_score * context_ptr->score_th[4]) / 100 &&
                    context_ptr->score_th[4] != 0) ||
                   context_ptr->number_of_segments == 5 || context_ptr->score_th[5] == 100) {
            context_ptr->sb_cost_array[sb_index] = context_ptr->interval_cost[4];
            context_ptr->predicted_cost += context_ptr->interval_cost[4];
        } else if ((score_to_min <= (max_to_min_score * context_ptr->score_th[5]) / 100 &&
                    context_ptr->score_th[5] != 0) ||
                   context_ptr->number_of_segments == 6 || context_ptr->score_th[6] == 100) {
            context_ptr->sb_cost_array[sb_index] = context_ptr->interval_cost[5];
            context_ptr->predicted_cost += context_ptr->interval_cost[5];
        } else {
            context_ptr->sb_cost_array[sb_index] = context_ptr->interval_cost[6];
            context_ptr->predicted_cost += context_ptr->interval_cost[6];
        }
    }
}

/******************************************************
* Loop multiple times over the SBs in order to derive the optimal budget per SB
    Input   : budget per picture, ditortion, detection signals, iteration
    Output  : optimal budget for each SB
******************************************************/
void derive_optimal_budget_per_sb(SequenceControlSet *scs_ptr, PictureControlSet *pcs_ptr,
                                  ModeDecisionConfigurationContext *context_ptr) {
    uint32_t sb_index;
    // Initialize the deviation between the picture predicted cost & the target budget to 100,
    uint32_t deviation_to_target = 1000;

    // Set the adjustment step to 1 (could be increased for faster convergence),
    int8_t adjustement_step = 1;

    // Set the initial shooting state & the final shooting state to TBD
    uint32_t initial_shooting = TBD_SHOOTING;
    uint32_t final_shooting   = TBD_SHOOTING;

    uint8_t max_adjustement_iteration = 100;
    uint8_t adjustement_iteration     = 0;

    while (deviation_to_target != 0 && (initial_shooting == final_shooting) &&
           adjustement_iteration <= max_adjustement_iteration) {
        if (context_ptr->predicted_cost < context_ptr->budget)
            initial_shooting = UNDER_SHOOTING;
        else
            initial_shooting = OVER_SHOOTING;
        // reset running cost
        context_ptr->predicted_cost = 0;

        for (sb_index = 0; sb_index < pcs_ptr->sb_total_count_pix; sb_index++) {
            SuperBlock *sb_ptr = pcs_ptr->sb_ptr_array[sb_index];

            set_sb_budget(scs_ptr, pcs_ptr, sb_ptr, context_ptr);
        }

        // Compute the deviation between the predicted budget & the target budget
        deviation_to_target =
            (ABS((int32_t)(context_ptr->predicted_cost - context_ptr->budget)) * 1000) /
            context_ptr->budget;
        // Derive shooting status
        if (context_ptr->predicted_cost < context_ptr->budget) {
            context_ptr->score_th[0] = MAX((context_ptr->score_th[0] - adjustement_step), 0);
            context_ptr->score_th[1] = MAX((context_ptr->score_th[1] - adjustement_step), 0);
            context_ptr->score_th[2] = MAX((context_ptr->score_th[2] - adjustement_step), 0);
            context_ptr->score_th[3] = MAX((context_ptr->score_th[3] - adjustement_step), 0);
            context_ptr->score_th[4] = MAX((context_ptr->score_th[4] - adjustement_step), 0);
            final_shooting           = UNDER_SHOOTING;
        } else {
            context_ptr->score_th[0] = (context_ptr->score_th[0] == 0)
                                           ? 0
                                           : MIN(context_ptr->score_th[0] + adjustement_step, 100);
            context_ptr->score_th[1] = (context_ptr->score_th[1] == 0)
                                           ? 0
                                           : MIN(context_ptr->score_th[1] + adjustement_step, 100);
            context_ptr->score_th[2] = (context_ptr->score_th[2] == 0)
                                           ? 0
                                           : MIN(context_ptr->score_th[2] + adjustement_step, 100);
            context_ptr->score_th[3] = (context_ptr->score_th[3] == 0)
                                           ? 0
                                           : MIN(context_ptr->score_th[3] + adjustement_step, 100);
            context_ptr->score_th[4] = (context_ptr->score_th[4] == 0)
                                           ? 0
                                           : MIN(context_ptr->score_th[4] + adjustement_step, 100);
            final_shooting = OVER_SHOOTING;
        }

        if (adjustement_iteration == 0) initial_shooting = final_shooting;

        adjustement_iteration++;
    }
}

EbErrorType derive_default_segments(SequenceControlSet *              scs_ptr,
                                    ModeDecisionConfigurationContext *context_ptr) {
    EbErrorType return_error = EB_ErrorNone;

    if (context_ptr->budget > (uint16_t)(scs_ptr->sb_tot_cnt * U_140)) {
        context_ptr->number_of_segments = 2;
        context_ptr->score_th[0]        = (int8_t)((1 * 100) / context_ptr->number_of_segments);
        context_ptr->score_th[1]        = (int8_t)((2 * 100) / context_ptr->number_of_segments);
        context_ptr->score_th[2]        = (int8_t)((3 * 100) / context_ptr->number_of_segments);
        context_ptr->score_th[3]        = (int8_t)((4 * 100) / context_ptr->number_of_segments);
        context_ptr->interval_cost[0]   = context_ptr->cost_depth_mode[SB_OPEN_LOOP_DEPTH_MODE - 1];
        context_ptr->interval_cost[1] =
            context_ptr->cost_depth_mode[SB_SQ_NON4_BLOCKS_DEPTH_MODE - 1];
    } else if (context_ptr->budget > (uint16_t)(scs_ptr->sb_tot_cnt * U_115)) {
        context_ptr->number_of_segments = 3;

        context_ptr->score_th[0] = (int8_t)((1 * 100) / context_ptr->number_of_segments);
        context_ptr->score_th[1] = (int8_t)((2 * 100) / context_ptr->number_of_segments);
        context_ptr->score_th[2] = (int8_t)((3 * 100) / context_ptr->number_of_segments);
        context_ptr->score_th[3] = (int8_t)((4 * 100) / context_ptr->number_of_segments);

        context_ptr->interval_cost[0] =
            context_ptr->cost_depth_mode[SB_FAST_OPEN_LOOP_DEPTH_MODE - 1];
        context_ptr->interval_cost[1] = context_ptr->cost_depth_mode[SB_OPEN_LOOP_DEPTH_MODE - 1];
        context_ptr->interval_cost[2] =
            context_ptr->cost_depth_mode[SB_SQ_NON4_BLOCKS_DEPTH_MODE - 1];
    } else {
        context_ptr->number_of_segments = 4;

        context_ptr->score_th[0] = (int8_t)((1 * 100) / context_ptr->number_of_segments);
        context_ptr->score_th[1] = (int8_t)((2 * 100) / context_ptr->number_of_segments);
        context_ptr->score_th[2] = (int8_t)((3 * 100) / context_ptr->number_of_segments);
        context_ptr->score_th[3] = (int8_t)((4 * 100) / context_ptr->number_of_segments);

        context_ptr->interval_cost[0] =
            context_ptr->cost_depth_mode[SB_PRED_OPEN_LOOP_DEPTH_MODE - 1];
        context_ptr->interval_cost[1] =
            context_ptr->cost_depth_mode[SB_FAST_OPEN_LOOP_DEPTH_MODE - 1];
        context_ptr->interval_cost[2] = context_ptr->cost_depth_mode[SB_OPEN_LOOP_DEPTH_MODE - 1];
        context_ptr->interval_cost[3] =
            context_ptr->cost_depth_mode[SB_SQ_NON4_BLOCKS_DEPTH_MODE - 1];
    }

    return return_error;
}
/******************************************************
* Compute the score of each SB
    Input   : distortion, detection signals
    Output  : SB score
******************************************************/
void derive_sb_score(PictureControlSet *pcs_ptr,
                     ModeDecisionConfigurationContext *context_ptr) {
    uint32_t sb_index;
    uint32_t sb_score = 0;
    uint32_t distortion;
    uint64_t sb_tot_score     = 0;
    context_ptr->sb_min_score = ~0u;
    context_ptr->sb_max_score = 0u;

    for (sb_index = 0; sb_index < pcs_ptr->sb_total_count_pix; sb_index++) {
        SbParams *sb_params = &pcs_ptr->parent_pcs_ptr->sb_params_array[sb_index];
        if (pcs_ptr->slice_type == I_SLICE)
            assert(0);
        else {
            if (sb_params->raster_scan_blk_validity[RASTER_SCAN_CU_INDEX_64x64] == EB_FALSE) {
                uint8_t blk8x8_index;
                uint8_t valid_blk_8x8_count = 0;
                distortion                  = 0;
                for (blk8x8_index = RASTER_SCAN_CU_INDEX_8x8_0;
                     blk8x8_index <= RASTER_SCAN_CU_INDEX_8x8_63;
                     blk8x8_index++) {
                    if (sb_params->raster_scan_blk_validity[blk8x8_index]) {
                        distortion = pcs_ptr->parent_pcs_ptr->me_results[sb_index]
                                         ->me_candidate[blk8x8_index][0]
                                         .distortion;
                        valid_blk_8x8_count++;
                    }
                }
                if (valid_blk_8x8_count > 0) distortion = (distortion / valid_blk_8x8_count) * 64;

                // Do not perform SB score manipulation for incomplete SBs as not valid signals
                sb_score = distortion;
            } else {
                distortion = pcs_ptr->parent_pcs_ptr->me_results[sb_index]
                                 ->me_candidate[RASTER_SCAN_CU_INDEX_64x64][0]
                                 .distortion;
                // Perform SB score manipulation for incomplete SBs for SQ mode
                sb_score = distortion;
            }
        }
        context_ptr->sb_score_array[sb_index] = sb_score;
        // Track MIN and MAX SB scores
        context_ptr->sb_min_score = MIN(sb_score, context_ptr->sb_min_score);
        context_ptr->sb_max_score = MAX(sb_score, context_ptr->sb_max_score);
        sb_tot_score += sb_score;
    }
    context_ptr->sb_average_score = (uint32_t)(sb_tot_score / pcs_ptr->sb_total_count_pix);
}

/******************************************************
* Set the target budget
Input   : cost per depth
Output  : budget per picture
******************************************************/
void set_target_budget_oq(PictureControlSet *pcs_ptr,
                          ModeDecisionConfigurationContext *context_ptr) {
    uint32_t budget;

    // Luminosity-based budget boost - if P or b only; add 1 U for each 1 current-to-ref diff
    uint32_t luminosity_change_boost = 0;
    if (pcs_ptr->slice_type != I_SLICE) {
        if (pcs_ptr->parent_pcs_ptr->is_used_as_reference_flag) {
            EbReferenceObject *ref_obj__l0, *ref_obj__l1;
            ref_obj__l0 =
                (EbReferenceObject *)pcs_ptr->ref_pic_ptr_array[REF_LIST_0][0]->object_ptr;
            ref_obj__l1 =
                (pcs_ptr->parent_pcs_ptr->slice_type == B_SLICE)
                    ? (EbReferenceObject *)pcs_ptr->ref_pic_ptr_array[REF_LIST_1][0]->object_ptr
                    : (EbReferenceObject *)EB_NULL;
            luminosity_change_boost =
                ABS(pcs_ptr->parent_pcs_ptr->average_intensity[0] - ref_obj__l0->average_intensity);
            luminosity_change_boost += (ref_obj__l1 != EB_NULL)
                                           ? ABS(pcs_ptr->parent_pcs_ptr->average_intensity[0] -
                                                 ref_obj__l1->average_intensity)
                                           : 0;
            luminosity_change_boost = MAX(MAX_LUMINOSITY_BOOST, luminosity_change_boost);
        }
    }
    // Hsan: cross multiplication to derive budget_per_sb from sb_average_score; budget_per_sb range is [SB_PRED_OPEN_LOOP_COST,U_150], and sb_average_score range [0,HIGH_SB_SCORE]
    // Hsan: 3 segments [0,LOW_SB_SCORE], [LOW_SB_SCORE,MEDIUM_SB_SCORE] and [MEDIUM_SB_SCORE,U_150]
    uint32_t budget_per_sb;
    if (context_ptr->sb_average_score <= LOW_SB_SCORE)
        budget_per_sb =
            ((context_ptr->sb_average_score * (SB_OPEN_LOOP_COST - SB_PRED_OPEN_LOOP_COST)) /
             LOW_SB_SCORE) +
            SB_PRED_OPEN_LOOP_COST;
    else if (context_ptr->sb_average_score <= MEDIUM_SB_SCORE)
        budget_per_sb =
            (((context_ptr->sb_average_score - LOW_SB_SCORE) * (U_125 - SB_OPEN_LOOP_COST)) /
             (MEDIUM_SB_SCORE - LOW_SB_SCORE)) +
            SB_OPEN_LOOP_COST;
    else
        budget_per_sb = (((context_ptr->sb_average_score - MEDIUM_SB_SCORE) * (U_150 - U_125)) /
                         (HIGH_SB_SCORE - MEDIUM_SB_SCORE)) +
                        U_125;
    budget_per_sb = CLIP3(
        SB_PRED_OPEN_LOOP_COST,
        U_150,
        budget_per_sb + budget_per_sb_boost[context_ptr->adp_level] + luminosity_change_boost);

    //SVT_LOG("picture_number = %d\tsb_average_score = %d\n", pcs_ptr->picture_number, budget_per_sb);
    budget = pcs_ptr->sb_total_count_pix * budget_per_sb;

    context_ptr->budget = budget;
}

/******************************************************
* Assign a search method for each SB
    Input   : SB score, detection signals
    Output  : search method for each SB
******************************************************/
void derive_sb_md_mode(SequenceControlSet *scs_ptr, PictureControlSet *pcs_ptr,
                       ModeDecisionConfigurationContext *context_ptr) {
    // Configure ADP
    configure_adp(pcs_ptr, context_ptr);

    // Derive SB score
    derive_sb_score(pcs_ptr, context_ptr);

    // Set the target budget
    set_target_budget_oq(pcs_ptr, context_ptr);

    // Set the percentage based thresholds
    derive_default_segments(scs_ptr, context_ptr);
    // Perform Budgetting
    derive_optimal_budget_per_sb(scs_ptr, pcs_ptr, context_ptr);

    // Set the search method using the SB cost (mapping)
    derive_search_method(pcs_ptr, context_ptr);
}

/******************************************************
* Derive Mode Decision Config Settings for OQ
Input   : encoder mode and tune
Output  : EncDec Kernel signal(s)
******************************************************/
EbErrorType signal_derivation_mode_decision_config_kernel_oq(
    SequenceControlSet *scs_ptr, PictureControlSet *pcs_ptr,
    ModeDecisionConfigurationContext *context_ptr) {
    UNUSED(scs_ptr);
    EbErrorType return_error = EB_ErrorNone;

    context_ptr->adp_level = pcs_ptr->parent_pcs_ptr->enc_mode;

    if (pcs_ptr->parent_pcs_ptr->sc_content_detected)
        if (pcs_ptr->enc_mode <= ENC_M6)
            pcs_ptr->update_cdf = 1;
        else
            pcs_ptr->update_cdf = 0;
    else
        pcs_ptr->update_cdf = (pcs_ptr->parent_pcs_ptr->enc_mode <= ENC_M5) ? 1 : 0;
    if (pcs_ptr->update_cdf) assert(scs_ptr->cdf_mode == 0 && "use cdf_mode 0");
    //Filter Intra Mode : 0: OFF  1: ON
    if (scs_ptr->seq_header.enable_filter_intra)
        pcs_ptr->pic_filter_intra_mode =
            pcs_ptr->parent_pcs_ptr->sc_content_detected == 0 && pcs_ptr->temporal_layer_index == 0
                ? 1
                : 0;
    else
        pcs_ptr->pic_filter_intra_mode = 0;
    FrameHeader *frm_hdr = &pcs_ptr->parent_pcs_ptr->frm_hdr;
    frm_hdr->allow_high_precision_mv =
        pcs_ptr->enc_mode <= ENC_M7 &&
                frm_hdr->quantization_params.base_q_idx < HIGH_PRECISION_MV_QTHRESH &&
                (scs_ptr->input_resolution == INPUT_SIZE_576p_RANGE_OR_LOWER)
            ? 1
            : 0;
    EbBool enable_wm;
    if (pcs_ptr->parent_pcs_ptr->sc_content_detected)
        enable_wm = EB_FALSE;
    else
        enable_wm = (pcs_ptr->parent_pcs_ptr->enc_mode == ENC_M0 ||
                     (pcs_ptr->parent_pcs_ptr->enc_mode <= ENC_M5 &&
                      pcs_ptr->parent_pcs_ptr->temporal_layer_index == 0))
                        ? EB_TRUE
                        : EB_FALSE;
    frm_hdr->allow_warped_motion =
        enable_wm &&
        !(frm_hdr->frame_type == KEY_FRAME || frm_hdr->frame_type == INTRA_ONLY_FRAME) &&
        !frm_hdr->error_resilient_mode;
    frm_hdr->is_motion_mode_switchable = frm_hdr->allow_warped_motion;
    // OBMC Level                                   Settings
    // 0                                            OFF
    // 1                                            OBMC @(MVP, PME and ME) + 16 NICs
    // 2                                            OBMC @(MVP, PME and ME) + Opt NICs
    // 3                                            OBMC @(MVP, PME ) + Opt NICs
    // 4                                            OBMC @(MVP, PME ) + Opt2 NICs
    if (scs_ptr->static_config.enable_obmc) {
        if (pcs_ptr->parent_pcs_ptr->enc_mode == ENC_M0)
            pcs_ptr->parent_pcs_ptr->pic_obmc_mode = pcs_ptr->slice_type != I_SLICE ? 2 : 0;
        else if (pcs_ptr->parent_pcs_ptr->enc_mode <= ENC_M2)
            pcs_ptr->parent_pcs_ptr->pic_obmc_mode =
                pcs_ptr->parent_pcs_ptr->sc_content_detected == 0 && pcs_ptr->slice_type != I_SLICE
                    ? 2
                    : 0;
        else
            pcs_ptr->parent_pcs_ptr->pic_obmc_mode = 0;

#if MR_MODE
        pcs_ptr->parent_pcs_ptr->pic_obmc_mode =
            pcs_ptr->parent_pcs_ptr->sc_content_detected == 0 && pcs_ptr->slice_type != I_SLICE ? 1
                                                                                                : 0;
#endif
    } else
        pcs_ptr->parent_pcs_ptr->pic_obmc_mode = 0;

    frm_hdr->is_motion_mode_switchable =
        frm_hdr->is_motion_mode_switchable || pcs_ptr->parent_pcs_ptr->pic_obmc_mode;

    return return_error;
}

void forward_sq_non4_blocks_to_md(SequenceControlSet *scs_ptr, PictureControlSet *pcs_ptr) {
    uint32_t sb_index;
    EbBool   split_flag;

    for (sb_index = 0; sb_index < pcs_ptr->sb_total_count_pix; ++sb_index) {
        MdcSbData *results_ptr = &pcs_ptr->mdc_sb_array[sb_index];

        results_ptr->leaf_count = 0;

        uint32_t blk_index =
            pcs_ptr->slice_type == I_SLICE && scs_ptr->seq_header.sb_size == BLOCK_128X128 ? 17 : 0;

        while (blk_index < scs_ptr->max_block_cnt) {
            split_flag = EB_TRUE;

            const BlockGeom *blk_geom = get_blk_geom_mds(blk_index);

            //if the parentSq is inside inject this block
            if (pcs_ptr->parent_pcs_ptr->sb_geom[sb_index].block_is_inside_md_scan[blk_index])

            {
                //int32_t offset_d1 = ns_blk_offset[(int32_t)from_shape_to_part[blk_geom->shape]]; //blk_ptr->best_d1_blk; // TOCKECK
                //int32_t num_d1_block = ns_blk_num[(int32_t)from_shape_to_part[blk_geom->shape]]; // context_ptr->blk_geom->totns; // TOCKECK
                //
                //                                                  // for (int32_t d1_itr = blk_it; d1_itr < blk_it + num_d1_block; d1_itr++) {
                // for (int32_t d1_itr = (int32_t)blk_index ; d1_itr < (int32_t)blk_index +  num_d1_block ; d1_itr++) {
                results_ptr->leaf_data_array[results_ptr->leaf_count].tot_d1_blocks = 1;

                results_ptr->leaf_data_array[results_ptr->leaf_count].leaf_index =
                    0; //valid only for square 85 world. will be removed.
                results_ptr->leaf_data_array[results_ptr->leaf_count].mds_idx = blk_index;

                if (blk_geom->sq_size > 8) {
                    results_ptr->leaf_data_array[results_ptr->leaf_count++].split_flag = EB_TRUE;
                    split_flag                                                         = EB_TRUE;
                } else {
                    results_ptr->leaf_data_array[results_ptr->leaf_count++].split_flag = EB_FALSE;
                    split_flag                                                         = EB_FALSE;
                }
            }

            blk_index +=
                split_flag
                    ? d1_depth_offset[scs_ptr->seq_header.sb_size == BLOCK_128X128][blk_geom->depth]
                    : ns_depth_offset[scs_ptr->seq_header.sb_size == BLOCK_128X128]
                                     [blk_geom->depth];
        }
    }

    pcs_ptr->parent_pcs_ptr->average_qp = (uint8_t)pcs_ptr->parent_pcs_ptr->picture_qp;
}

void sb_forward_sq_non4_blocks_to_md(SequenceControlSet *scs_ptr, PictureControlSet *pcs_ptr,
                                     uint32_t sb_index) {
    EbBool     split_flag;
    MdcSbData *results_ptr  = &pcs_ptr->mdc_sb_array[sb_index];
    results_ptr->leaf_count = 0;
    uint32_t blk_index =
        pcs_ptr->slice_type == I_SLICE && scs_ptr->seq_header.sb_size == BLOCK_128X128 ? 17 : 0;

    while (blk_index < scs_ptr->max_block_cnt) {
        split_flag                = EB_TRUE;
        const BlockGeom *blk_geom = get_blk_geom_mds(blk_index);
        if (pcs_ptr->parent_pcs_ptr->sb_geom[sb_index].block_is_inside_md_scan[blk_index]) {
            results_ptr->leaf_data_array[results_ptr->leaf_count].tot_d1_blocks = 1;

            results_ptr->leaf_data_array[results_ptr->leaf_count].leaf_index =
                0; //valid only for square 85 world. will be removed.
            results_ptr->leaf_data_array[results_ptr->leaf_count].mds_idx = blk_index;

            if (blk_geom->sq_size > 8) {
                results_ptr->leaf_data_array[results_ptr->leaf_count++].split_flag = EB_TRUE;
                split_flag                                                         = EB_TRUE;
            } else {
                results_ptr->leaf_data_array[results_ptr->leaf_count++].split_flag = EB_FALSE;
                split_flag                                                         = EB_FALSE;
            }
        }
        blk_index +=
            split_flag
                ? d1_depth_offset[scs_ptr->seq_header.sb_size == BLOCK_128X128][blk_geom->depth]
                : ns_depth_offset[scs_ptr->seq_header.sb_size == BLOCK_128X128][blk_geom->depth];
    }
    pcs_ptr->parent_pcs_ptr->average_qp = (uint8_t)pcs_ptr->parent_pcs_ptr->picture_qp;
}

void forward_all_c_blocks_to_md(SequenceControlSet *scs_ptr, PictureControlSet *pcs_ptr) {
    uint32_t sb_index;
    for (sb_index = 0; sb_index < pcs_ptr->sb_total_count_pix; ++sb_index) {
        MdcSbData *results_ptr  = &pcs_ptr->mdc_sb_array[sb_index];
        results_ptr->leaf_count = 0;
        uint32_t blk_index      = 0;
        uint32_t tot_d1_blocks;

        while (blk_index < scs_ptr->max_block_cnt) {
            tot_d1_blocks             = 0;
            const BlockGeom *blk_geom = get_blk_geom_mds(blk_index);

            //if the parentSq is inside inject this block
            uint8_t is_blk_allowed =
                pcs_ptr->slice_type != I_SLICE ? 1 : (blk_geom->sq_size < 128) ? 1 : 0;

            if (pcs_ptr->parent_pcs_ptr->sb_geom[sb_index].block_is_inside_md_scan[blk_index] && is_blk_allowed) {
                tot_d1_blocks =
                    results_ptr->leaf_data_array[results_ptr->leaf_count].tot_d1_blocks =

                        blk_geom->sq_size == 128
                            ? 17
                            : blk_geom->sq_size > 16
                                  ? 25
                                  : blk_geom->sq_size == 16 ? 17 : blk_geom->sq_size == 8 ? 1 : 1;

                for (uint32_t idx = 0; idx < tot_d1_blocks; ++idx) {
                    blk_geom = get_blk_geom_mds(blk_index);

                    //if the parentSq is inside inject this block
                    if (pcs_ptr->parent_pcs_ptr->sb_geom[sb_index].block_is_inside_md_scan[blk_index]) {
                        results_ptr->leaf_data_array[results_ptr->leaf_count].leaf_index =
                            0; //valid only for square 85 world. will be removed.
                        results_ptr->leaf_data_array[results_ptr->leaf_count].mds_idx = blk_index;
                        if (blk_geom->sq_size > 4)
                            results_ptr->leaf_data_array[results_ptr->leaf_count++].split_flag =
                                EB_TRUE;
                        else
                            results_ptr->leaf_data_array[results_ptr->leaf_count++].split_flag =
                                EB_FALSE;
                    }
                    blk_index++;
                }
            }
            blk_index +=
                (d1_depth_offset[scs_ptr->seq_header.sb_size == BLOCK_128X128][blk_geom->depth] -
                 tot_d1_blocks);
        }
    }

    pcs_ptr->parent_pcs_ptr->average_qp = (uint8_t)pcs_ptr->parent_pcs_ptr->picture_qp;
}
void av1_set_ref_frame(MvReferenceFrame *rf, int8_t ref_frame_type);

static INLINE int get_relative_dist(const OrderHintInfo *oh, int a, int b) {
    if (!oh->enable_order_hint) return 0;

    const int bits = oh->order_hint_bits;

    assert(bits >= 1);
    assert(a >= 0 && a < (1 << bits));
    assert(b >= 0 && b < (1 << bits));

    int       diff = a - b;
    const int m    = 1 << (bits - 1);
    diff           = (diff & (m - 1)) - (diff & m);
    return diff;
}

static int get_block_position(Av1Common *cm, int *mi_r, int *mi_c, int blk_row, int blk_col, MV mv,
                              int sign_bias) {
    const int base_blk_row = (blk_row >> 3) << 3;
    const int base_blk_col = (blk_col >> 3) << 3;

    const int row_offset =
        (mv.row >= 0) ? (mv.row >> (4 + MI_SIZE_LOG2)) : -((-mv.row) >> (4 + MI_SIZE_LOG2));

    const int col_offset =
        (mv.col >= 0) ? (mv.col >> (4 + MI_SIZE_LOG2)) : -((-mv.col) >> (4 + MI_SIZE_LOG2));

    const int row = (sign_bias == 1) ? blk_row - row_offset : blk_row + row_offset;
    const int col = (sign_bias == 1) ? blk_col - col_offset : blk_col + col_offset;

    if (row < 0 || row >= (cm->mi_rows >> 1) || col < 0 || col >= (cm->mi_cols >> 1)) return 0;

    if (row < base_blk_row - (MAX_OFFSET_HEIGHT >> 3) ||
        row >= base_blk_row + 8 + (MAX_OFFSET_HEIGHT >> 3) ||
        col < base_blk_col - (MAX_OFFSET_WIDTH >> 3) ||
        col >= base_blk_col + 8 + (MAX_OFFSET_WIDTH >> 3))
        return 0;

    *mi_r = row;
    *mi_c = col;

    return 1;
}

#define MFMV_STACK_SIZE 3

// Note: motion_filed_projection finds motion vectors of current frame's
// reference frame, and projects them to current frame. To make it clear,
// let's call current frame's reference frame as start frame.
// Call Start frame's reference frames as reference frames.
// Call ref_offset as frame distances between start frame and its reference
// frames.
static int motion_field_projection(Av1Common *cm, PictureControlSet *pcs_ptr,
                                   MvReferenceFrame start_frame, int dir) {
    TPL_MV_REF *tpl_mvs_base           = pcs_ptr->tpl_mvs;
    int         ref_offset[REF_FRAMES] = {0};

    MvReferenceFrame rf[2];
    av1_set_ref_frame(rf, start_frame);

    uint8_t list_idx0, ref_idx_l0;
    list_idx0  = get_list_idx(start_frame);
    ref_idx_l0 = get_ref_frame_idx(start_frame);
    EbReferenceObject *start_frame_buf =
        (EbReferenceObject *)pcs_ptr->ref_pic_ptr_array[list_idx0][ref_idx_l0]->object_ptr;

    if (start_frame_buf == NULL) return 0;

    if (start_frame_buf->frame_type == KEY_FRAME || start_frame_buf->frame_type == INTRA_ONLY_FRAME)
        return 0;

    const int                 start_frame_order_hint = start_frame_buf->order_hint;
    const unsigned int *const ref_order_hints        = &start_frame_buf->ref_order_hint[0];
    const int                 cur_order_hint         = pcs_ptr->parent_pcs_ptr->cur_order_hint;
    int                       start_to_current_frame_offset =
        get_relative_dist(&pcs_ptr->parent_pcs_ptr->scs_ptr->seq_header.order_hint_info,
                          start_frame_order_hint,
                          cur_order_hint);

    for (MvReferenceFrame rf = LAST_FRAME; rf <= INTER_REFS_PER_FRAME; ++rf) {
        ref_offset[rf] =
            get_relative_dist(&pcs_ptr->parent_pcs_ptr->scs_ptr->seq_header.order_hint_info,
                              start_frame_order_hint,
                              ref_order_hints[rf - LAST_FRAME]);
    }

    if (dir == 2) start_to_current_frame_offset = -start_to_current_frame_offset;

    MV_REF *  mv_ref_base = start_frame_buf->mvs;
    const int mvs_rows    = (cm->mi_rows + 1) >> 1;
    const int mvs_cols    = (cm->mi_cols + 1) >> 1;

    for (int blk_row = 0; blk_row < mvs_rows; ++blk_row) {
        for (int blk_col = 0; blk_col < mvs_cols; ++blk_col) {
            MV_REF *mv_ref = &mv_ref_base[blk_row * mvs_cols + blk_col];
            MV      fwd_mv = mv_ref->mv.as_mv;

            if (mv_ref->ref_frame > INTRA_FRAME) {
                IntMv     this_mv;
                int       mi_r, mi_c;
                const int ref_frame_offset = ref_offset[mv_ref->ref_frame];

                int pos_valid = abs(ref_frame_offset) <= MAX_FRAME_DISTANCE &&
                                ref_frame_offset > 0 &&
                                abs(start_to_current_frame_offset) <= MAX_FRAME_DISTANCE;

                if (pos_valid) {
                    get_mv_projection(
                        &this_mv.as_mv, fwd_mv, start_to_current_frame_offset, ref_frame_offset);
                    pos_valid = get_block_position(
                        cm, &mi_r, &mi_c, blk_row, blk_col, this_mv.as_mv, dir >> 1);
                }

                if (pos_valid) {
                    const int mi_offset = mi_r * (cm->mi_stride >> 1) + mi_c;

                    tpl_mvs_base[mi_offset].mfmv0.as_mv.row  = fwd_mv.row;
                    tpl_mvs_base[mi_offset].mfmv0.as_mv.col  = fwd_mv.col;
                    tpl_mvs_base[mi_offset].ref_frame_offset = ref_frame_offset;
                }
            }
        }
    }

    return 1;
}
void av1_setup_motion_field(Av1Common *cm, PictureControlSet *pcs_ptr) {
    const OrderHintInfo *const order_hint_info =
        &pcs_ptr->parent_pcs_ptr->scs_ptr->seq_header.order_hint_info;
    memset(pcs_ptr->ref_frame_side, 0, sizeof(pcs_ptr->ref_frame_side));
    if (!order_hint_info->enable_order_hint) return;

    TPL_MV_REF *tpl_mvs_base = pcs_ptr->tpl_mvs;
    int         size         = ((cm->mi_rows + MAX_MIB_SIZE) >> 1) * (cm->mi_stride >> 1);
    for (int idx = 0; idx < size; ++idx) {
        tpl_mvs_base[idx].mfmv0.as_int     = INVALID_MV;
        tpl_mvs_base[idx].ref_frame_offset = 0;
    }

    const int                cur_order_hint = pcs_ptr->parent_pcs_ptr->cur_order_hint;
    const EbReferenceObject *ref_buf[INTER_REFS_PER_FRAME];
    int                      ref_order_hint[INTER_REFS_PER_FRAME];

    for (int ref_frame = LAST_FRAME; ref_frame <= ALTREF_FRAME; ref_frame++) {
        const int ref_idx    = ref_frame - LAST_FRAME;
        int       order_hint = 0;
        uint8_t   list_idx0, ref_idx_l0;
        list_idx0  = get_list_idx(ref_frame);
        ref_idx_l0 = get_ref_frame_idx(ref_frame);
        EbReferenceObject *buf =
            (EbReferenceObject *)pcs_ptr->ref_pic_ptr_array[list_idx0][ref_idx_l0]->object_ptr;

        if (buf != NULL) order_hint = buf->order_hint;

        ref_buf[ref_idx]        = buf;
        ref_order_hint[ref_idx] = order_hint;

        if (get_relative_dist(order_hint_info, order_hint, cur_order_hint) > 0)
            pcs_ptr->ref_frame_side[ref_frame] = 1;
        else if (order_hint == cur_order_hint)
            pcs_ptr->ref_frame_side[ref_frame] = -1;
    }

    int ref_stamp = MFMV_STACK_SIZE - 1;

    if (ref_buf[LAST_FRAME - LAST_FRAME] != NULL) {
        const int alt_of_lst_order_hint =
            ref_buf[LAST_FRAME - LAST_FRAME]->ref_order_hint[ALTREF_FRAME - LAST_FRAME];
        const int is_lst_overlay =
            (alt_of_lst_order_hint == ref_order_hint[GOLDEN_FRAME - LAST_FRAME]);
        if (!is_lst_overlay) motion_field_projection(cm, pcs_ptr, LAST_FRAME, 2);

        --ref_stamp;
    }

    if (get_relative_dist(
            order_hint_info, ref_order_hint[BWDREF_FRAME - LAST_FRAME], cur_order_hint) > 0) {
        if (motion_field_projection(cm, pcs_ptr, BWDREF_FRAME, 0)) --ref_stamp;
    }

    if (get_relative_dist(
            order_hint_info, ref_order_hint[ALTREF2_FRAME - LAST_FRAME], cur_order_hint) > 0) {
        if (motion_field_projection(cm, pcs_ptr, ALTREF2_FRAME, 0)) --ref_stamp;
    }

    if (get_relative_dist(
            order_hint_info, ref_order_hint[ALTREF_FRAME - LAST_FRAME], cur_order_hint) > 0 &&
        ref_stamp >= 0)
        if (motion_field_projection(cm, pcs_ptr, ALTREF_FRAME, 0)) --ref_stamp;

    if (ref_stamp >= 0) motion_field_projection(cm, pcs_ptr, LAST2_FRAME, 2);
}
/******************************************************
 * Mode Decision Configuration Kernel
 ******************************************************/
void *mode_decision_configuration_kernel(void *input_ptr) {
    // Context & SCS & PCS
    EbThreadContext *                 thread_context_ptr = (EbThreadContext *)input_ptr;
    ModeDecisionConfigurationContext *context_ptr =
        (ModeDecisionConfigurationContext *)thread_context_ptr->priv;
    PictureControlSet * pcs_ptr;
    SequenceControlSet *scs_ptr;
    FrameHeader *       frm_hdr;
    // Input
    EbObjectWrapper *   rate_control_results_wrapper_ptr;
    RateControlResults *rate_control_results_ptr;

    // Output
    EbObjectWrapper *enc_dec_tasks_wrapper_ptr;
    EncDecTasks *    enc_dec_tasks_ptr;

    for (;;) {
        // Get RateControl Results
        eb_get_full_object(context_ptr->rate_control_input_fifo_ptr,
                           &rate_control_results_wrapper_ptr);

        rate_control_results_ptr =
            (RateControlResults *)rate_control_results_wrapper_ptr->object_ptr;
        pcs_ptr = (PictureControlSet *)rate_control_results_ptr->pcs_wrapper_ptr->object_ptr;
        scs_ptr = (SequenceControlSet *)pcs_ptr->scs_wrapper_ptr->object_ptr;
        if (pcs_ptr->parent_pcs_ptr->frm_hdr.use_ref_frame_mvs)
            av1_setup_motion_field(pcs_ptr->parent_pcs_ptr->av1_cm, pcs_ptr);

        frm_hdr = &pcs_ptr->parent_pcs_ptr->frm_hdr;

        // Mode Decision Configuration Kernel Signal(s) derivation
        signal_derivation_mode_decision_config_kernel_oq(scs_ptr, pcs_ptr, context_ptr);

        context_ptr->qp = pcs_ptr->picture_qp;

        pcs_ptr->parent_pcs_ptr->average_qp = 0;
        pcs_ptr->intra_coded_area           = 0;
        // Compute picture and slice level chroma QP offsets
        set_slice_and_picture_chroma_qp_offsets( // HT done
            pcs_ptr);

        // Compute Tc, and Beta offsets for a given picture
        // Set reference cdef strength
        set_reference_cdef_strength(pcs_ptr);

        // Set reference sg ep
        set_reference_sg_ep(pcs_ptr);
        set_global_motion_field(pcs_ptr);

        eb_av1_qm_init(pcs_ptr->parent_pcs_ptr);

        Quants *const   quants   = &pcs_ptr->parent_pcs_ptr->quants;
        Dequants *const dequants = &pcs_ptr->parent_pcs_ptr->deq;

        eb_av1_set_quantizer(pcs_ptr->parent_pcs_ptr, frm_hdr->quantization_params.base_q_idx);

        eb_av1_build_quantizer((AomBitDepth)scs_ptr->static_config.encoder_bit_depth,
                               frm_hdr->quantization_params.delta_q_dc[AOM_PLANE_Y],
                               frm_hdr->quantization_params.delta_q_dc[AOM_PLANE_U],
                               frm_hdr->quantization_params.delta_q_ac[AOM_PLANE_U],
                               frm_hdr->quantization_params.delta_q_dc[AOM_PLANE_V],
                               frm_hdr->quantization_params.delta_q_ac[AOM_PLANE_V],
                               quants,
                               dequants);

        Quants *const   quants_md   = &pcs_ptr->parent_pcs_ptr->quants_md;
        Dequants *const dequants_md = &pcs_ptr->parent_pcs_ptr->deq_md;
        eb_av1_build_quantizer(pcs_ptr->hbd_mode_decision ? AOM_BITS_10 : AOM_BITS_8,
                               frm_hdr->quantization_params.delta_q_dc[AOM_PLANE_Y],
                               frm_hdr->quantization_params.delta_q_dc[AOM_PLANE_U],
                               frm_hdr->quantization_params.delta_q_ac[AOM_PLANE_U],
                               frm_hdr->quantization_params.delta_q_dc[AOM_PLANE_V],
                               frm_hdr->quantization_params.delta_q_ac[AOM_PLANE_V],
                               quants_md,
                               dequants_md);

        // Hsan: collapse spare code
        MdRateEstimationContext *md_rate_estimation_array;
        uint32_t                 entropy_coding_qp;

        // QP
        context_ptr->qp = pcs_ptr->picture_qp;

        // QP Index
        context_ptr->qp_index = (uint8_t)frm_hdr->quantization_params.base_q_idx;

        // Lambda Assignement
        uint32_t lambda_sse;
        uint32_t lambdasad_;
        (*av1_lambda_assignment_function_table[pcs_ptr->parent_pcs_ptr->pred_structure])(
            &lambdasad_,
            &lambda_sse,
            &lambdasad_,
            &lambda_sse,
            (uint8_t)pcs_ptr->parent_pcs_ptr->enhanced_picture_ptr->bit_depth,
            context_ptr->qp_index,
            pcs_ptr->hbd_mode_decision);
        context_ptr->lambda      = (uint64_t)lambdasad_;
        md_rate_estimation_array = pcs_ptr->md_rate_estimation_array;
        // Reset MD rate Estimation table to initial values by copying from md_rate_estimation_array
        if (context_ptr->is_md_rate_estimation_ptr_owner) {
            EB_FREE_ARRAY(context_ptr->md_rate_estimation_ptr);
            context_ptr->is_md_rate_estimation_ptr_owner = EB_FALSE;
        }
        context_ptr->md_rate_estimation_ptr = md_rate_estimation_array;

        entropy_coding_qp = frm_hdr->quantization_params.base_q_idx;
        if (pcs_ptr->parent_pcs_ptr->frm_hdr.primary_ref_frame != PRIMARY_REF_NONE)
            memcpy(pcs_ptr->coeff_est_entropy_coder_ptr->fc,
                   &pcs_ptr->ref_frame_context[pcs_ptr->parent_pcs_ptr->frm_hdr.primary_ref_frame],
                   sizeof(FRAME_CONTEXT));
        else
            reset_entropy_coder(scs_ptr->encode_context_ptr,
                                pcs_ptr->coeff_est_entropy_coder_ptr,
                                entropy_coding_qp,
                                pcs_ptr->slice_type);

        // Initial Rate Estimation of the syntax elements
        av1_estimate_syntax_rate(md_rate_estimation_array,
                                 pcs_ptr->slice_type == I_SLICE ? EB_TRUE : EB_FALSE,
                                 pcs_ptr->coeff_est_entropy_coder_ptr->fc);
        // Initial Rate Estimation of the Motion vectors
        av1_estimate_mv_rate(
            pcs_ptr, md_rate_estimation_array, pcs_ptr->coeff_est_entropy_coder_ptr->fc);
        // Initial Rate Estimation of the quantized coefficients
        av1_estimate_coefficients_rate(md_rate_estimation_array,
                                       pcs_ptr->coeff_est_entropy_coder_ptr->fc);
        if (pcs_ptr->parent_pcs_ptr->pic_depth_mode == PIC_SB_SWITCH_DEPTH_MODE) {
            derive_sb_md_mode(scs_ptr, pcs_ptr, context_ptr);

            for (int sb_index = 0; sb_index < pcs_ptr->sb_total_count; ++sb_index) {
                if (pcs_ptr->parent_pcs_ptr->sb_depth_mode_array[sb_index] ==
                    SB_SQ_BLOCKS_DEPTH_MODE) {
                    sb_forward_sq_blocks_to_md(scs_ptr, pcs_ptr, sb_index);
                } else if (pcs_ptr->parent_pcs_ptr->sb_depth_mode_array[sb_index] ==
                           SB_SQ_NON4_BLOCKS_DEPTH_MODE) {
                    sb_forward_sq_non4_blocks_to_md(scs_ptr, pcs_ptr, sb_index);
                } else {
                    perform_early_sb_partitionning_sb(context_ptr, scs_ptr, pcs_ptr, sb_index);
                }
            }
        } else if (pcs_ptr->parent_pcs_ptr->pic_depth_mode == PIC_ALL_DEPTH_MODE ||
                   pcs_ptr->parent_pcs_ptr->pic_depth_mode == PIC_MULTI_PASS_PD_MODE_0 ||
                   pcs_ptr->parent_pcs_ptr->pic_depth_mode == PIC_MULTI_PASS_PD_MODE_1 ||
                   pcs_ptr->parent_pcs_ptr->pic_depth_mode == PIC_MULTI_PASS_PD_MODE_2 ||
                   pcs_ptr->parent_pcs_ptr->pic_depth_mode == PIC_MULTI_PASS_PD_MODE_3) {
            forward_all_blocks_to_md(scs_ptr, pcs_ptr);
        } else if (pcs_ptr->parent_pcs_ptr->pic_depth_mode == PIC_ALL_C_DEPTH_MODE) {
            forward_all_c_blocks_to_md(scs_ptr, pcs_ptr);
        } else if (pcs_ptr->parent_pcs_ptr->pic_depth_mode == PIC_SQ_DEPTH_MODE) {
            forward_sq_blocks_to_md(scs_ptr, pcs_ptr);
        } else if (pcs_ptr->parent_pcs_ptr->pic_depth_mode == PIC_SQ_NON4_DEPTH_MODE) {
            forward_sq_non4_blocks_to_md(scs_ptr, pcs_ptr);
        } else if (pcs_ptr->parent_pcs_ptr->pic_depth_mode >= PIC_OPEN_LOOP_DEPTH_MODE) {
            // Predict the SB partitionning
            perform_early_sb_partitionning( // HT done
                context_ptr,
                scs_ptr,
                pcs_ptr);
        } else { // (pcs_ptr->parent_pcs_ptr->mdMode == PICT_BDP_DEPTH_MODE || pcs_ptr->parent_pcs_ptr->mdMode == PICT_LIGHT_BDP_DEPTH_MODE )
            pcs_ptr->parent_pcs_ptr->average_qp = (uint8_t)pcs_ptr->parent_pcs_ptr->picture_qp;
        }
        if (frm_hdr->allow_intrabc) {
            int            i;
            int            speed          = 1;
            SpeedFeatures *sf             = &pcs_ptr->sf;
            sf->allow_exhaustive_searches = 1;

            const int mesh_speed = AOMMIN(speed, MAX_MESH_SPEED);
            //if (cpi->twopass.fr_content_type == FC_GRAPHICS_ANIMATION)
            //    sf->exhaustive_searches_thresh = (1 << 24);
            //else
            sf->exhaustive_searches_thresh = (1 << 25);

            sf->max_exaustive_pct = good_quality_max_mesh_pct[mesh_speed];
            if (mesh_speed > 0)
                sf->exhaustive_searches_thresh = sf->exhaustive_searches_thresh << 1;

            for (i = 0; i < MAX_MESH_STEP; ++i) {
                sf->mesh_patterns[i].range    = good_quality_mesh_patterns[mesh_speed][i].range;
                sf->mesh_patterns[i].interval = good_quality_mesh_patterns[mesh_speed][i].interval;
            }

            if (pcs_ptr->slice_type == I_SLICE) {
                for (i = 0; i < MAX_MESH_STEP; ++i) {
                    sf->mesh_patterns[i].range    = intrabc_mesh_patterns[mesh_speed][i].range;
                    sf->mesh_patterns[i].interval = intrabc_mesh_patterns[mesh_speed][i].interval;
                }
                sf->max_exaustive_pct = intrabc_max_mesh_pct[mesh_speed];
            }

            {
                // add to hash table
                const int pic_width = pcs_ptr->parent_pcs_ptr->av1_cm->frm_size.frame_width;
                const int pic_height = pcs_ptr->parent_pcs_ptr->av1_cm->frm_size.frame_height;

                uint32_t *block_hash_values[2][2];
                int8_t *  is_block_same[2][3];
                int       k, j;

                for (k = 0; k < 2; k++) {
                    for (j = 0; j < 2; j++)
                        block_hash_values[k][j] = malloc(sizeof(uint32_t) * pic_width * pic_height);
                    for (j = 0; j < 3; j++)
                        is_block_same[k][j] = malloc(sizeof(int8_t) * pic_width * pic_height);
                }

                //pcs_ptr->hash_table.p_lookup_table = NULL;
                //av1_hash_table_create(&pcs_ptr->hash_table);

                Yv12BufferConfig cpi_source;
                link_eb_to_aom_buffer_desc_8bit(pcs_ptr->parent_pcs_ptr->enhanced_picture_ptr,
                                                &cpi_source);

                av1_crc_calculator_init(&pcs_ptr->crc_calculator1, 24, 0x5D6DCB);
                av1_crc_calculator_init(&pcs_ptr->crc_calculator2, 24, 0x864CFB);

                av1_generate_block_2x2_hash_value(
                    &cpi_source, block_hash_values[0], is_block_same[0], pcs_ptr);
                av1_generate_block_hash_value(&cpi_source,
                                              4,
                                              block_hash_values[0],
                                              block_hash_values[1],
                                              is_block_same[0],
                                              is_block_same[1],
                                              pcs_ptr);
                av1_add_to_hash_map_by_row_with_precal_data(&pcs_ptr->hash_table,
                                                            block_hash_values[1],
                                                            is_block_same[1][2],
                                                            pic_width,
                                                            pic_height,
                                                            4);
                av1_generate_block_hash_value(&cpi_source,
                                              8,
                                              block_hash_values[1],
                                              block_hash_values[0],
                                              is_block_same[1],
                                              is_block_same[0],
                                              pcs_ptr);
                av1_add_to_hash_map_by_row_with_precal_data(&pcs_ptr->hash_table,
                                                            block_hash_values[0],
                                                            is_block_same[0][2],
                                                            pic_width,
                                                            pic_height,
                                                            8);
                av1_generate_block_hash_value(&cpi_source,
                                              16,
                                              block_hash_values[0],
                                              block_hash_values[1],
                                              is_block_same[0],
                                              is_block_same[1],
                                              pcs_ptr);
                av1_add_to_hash_map_by_row_with_precal_data(&pcs_ptr->hash_table,
                                                            block_hash_values[1],
                                                            is_block_same[1][2],
                                                            pic_width,
                                                            pic_height,
                                                            16);
                av1_generate_block_hash_value(&cpi_source,
                                              32,
                                              block_hash_values[1],
                                              block_hash_values[0],
                                              is_block_same[1],
                                              is_block_same[0],
                                              pcs_ptr);
                av1_add_to_hash_map_by_row_with_precal_data(&pcs_ptr->hash_table,
                                                            block_hash_values[0],
                                                            is_block_same[0][2],
                                                            pic_width,
                                                            pic_height,
                                                            32);
                av1_generate_block_hash_value(&cpi_source,
                                              64,
                                              block_hash_values[0],
                                              block_hash_values[1],
                                              is_block_same[0],
                                              is_block_same[1],
                                              pcs_ptr);
                av1_add_to_hash_map_by_row_with_precal_data(&pcs_ptr->hash_table,
                                                            block_hash_values[1],
                                                            is_block_same[1][2],
                                                            pic_width,
                                                            pic_height,
                                                            64);

                av1_generate_block_hash_value(&cpi_source,
                                              128,
                                              block_hash_values[1],
                                              block_hash_values[0],
                                              is_block_same[1],
                                              is_block_same[0],
                                              pcs_ptr);
                av1_add_to_hash_map_by_row_with_precal_data(&pcs_ptr->hash_table,
                                                            block_hash_values[0],
                                                            is_block_same[0][2],
                                                            pic_width,
                                                            pic_height,
                                                            128);

                for (k = 0; k < 2; k++) {
                    for (j = 0; j < 2; j++) free(block_hash_values[k][j]);
                    for (j = 0; j < 3; j++) free(is_block_same[k][j]);
                }
            }

            eb_av1_init3smotion_compensation(
                &pcs_ptr->ss_cfg, pcs_ptr->parent_pcs_ptr->enhanced_picture_ptr->stride_y);
        }

        // Derive MD parameters
        set_md_settings( // HT Done
            scs_ptr,
            pcs_ptr);

        // Post the results to the MD processes
        eb_get_empty_object(context_ptr->mode_decision_configuration_output_fifo_ptr,
                            &enc_dec_tasks_wrapper_ptr);

        enc_dec_tasks_ptr                  = (EncDecTasks *)enc_dec_tasks_wrapper_ptr->object_ptr;
        enc_dec_tasks_ptr->pcs_wrapper_ptr = rate_control_results_ptr->pcs_wrapper_ptr;
        enc_dec_tasks_ptr->input_type      = ENCDEC_TASKS_MDC_INPUT;

        // Post the Full Results Object
        eb_post_full_object(enc_dec_tasks_wrapper_ptr);

        // Release Rate Control Results
        eb_release_object(rate_control_results_wrapper_ptr);
    }

    return EB_NULL;
}
