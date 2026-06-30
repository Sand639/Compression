#include "compress.h"

// ==========================================================================
// コンテキストミキシング (CM) — 二値算術符号化 + 複数文脈モデル + ロジスティック混合
//   lpaq 系の軽量版。モデル: order-1/2/3 文脈 + マッチモデル。各モデルの予測を
//   stretch 領域で重み付き加算 (mixer) し squash で最終確率に。1 ビットごとに
//   実ビットで各重みとモデル確率を更新。エンコード/デコードは同一モデルを駆動し
//   符号器だけが異なるので完全可逆。出力: [u64 size][二値算術ストリーム]。
// ==========================================================================
static int CM_squash(int d) {
    static const int t[33] = {1,2,3,6,10,16,27,45,73,120,194,310,488,747,1101,1546,2047,
                              2549,2994,3348,3607,3785,3901,3975,4022,4050,4068,4079,4085,4089,4092,4093,4094};
    if (d >  2047) return 4095;
    if (d < -2047) return 0;
    int w = d & 127; d = (d >> 7) + 16;
    return (t[d] * (128 - w) + t[d + 1] * w + 64) >> 7;
}
struct CM_Stretch {
    int v[4096];
    CM_Stretch() {
        int pi = 0;
        for (int x = -2047; x <= 2047; ++x) { int val = CM_squash(x); for (int j = pi; j <= val; ++j) v[j] = x; pi = val + 1; }
        for (int j = pi; j < 4096; ++j) v[j] = 2047;
    }
};
static const CM_Stretch CM_STR;

// TeraPad.exe のBCJ後ストリームから学習した x86 operand prior。
// [rel32/imm32 × 残りバイト位置][bit-prefix c0] の P(1), 12bit。
static const uint16_t EXE_PRIOR[8][256] = {
 {2048,297,98,1881,36,1615,711,3103,14,2420,2030,2146,355,2460,1621,2558,26,2194,1805,2313,1719,1902,2239,1638,3072,1784,1588,2814,819,2101,3441,1851,8,2207,1236,1141,2873,1468,983,928,2884,2067,1174,2048,3324,1332,3563,2995,1489,367,918,2093,2350,2415,2963,1204,1220,2048,1580,1638,2457,445,1365,3180,6,2255,1322,2523,862,1927,1001,910,780,2560,2574,409,3036,1077,2533,534,2000,567,1496,1236,1101,1293,2299,1868,1170,2658,1077,2925,1462,186,862,2129,3029,591,2920,227,1335,1462,2493,1365,1213,113,2409,2389,1170,1872,2693,1321,737,2952,1293,2155,796,1958,885,1966,1780,963,1011,1280,277,630,918,3602,6,2048,1408,2415,930,2048,2234,2925,264,1365,819,1365,351,1024,1092,1638,1820,819,1077,924,2925,3561,2802,2730,1280,3630,1365,2048,955,3669,2925,1024,712,2792,3630,819,481,1228,1832,963,1484,3150,2578,2520,3465,3351,2816,1972,1489,2457,1365,1077,762,1280,3185,2730,1228,1365,1590,1638,1536,3276,2293,3185,1729,3900,3852,2184,3997,4032,3680,2457,384,768,2867,682,1638,1092,963,1820,2457,2275,2958,2048,1536,2978,1489,1911,744,819,2457,3373,2520,167,2234,1489,1529,3664,1575,1280,2925,1170,1638,1861,546,2048,1260,1706,546,3640,1462,2835,1170,1861,2275,1365,275,3392,712,2234,211,1638,184,1445,890,2925,655,3863},
 {2048,178,83,2467,72,1071,853,3351,87,3204,1853,2193,588,2095,1542,2166,794,288,2032,1759,1259,1960,1762,1861,2312,1153,768,2323,1454,2079,2651,3051,950,2652,227,2621,3040,1323,1411,2099,2973,2420,3396,1631,1966,2586,2829,2315,1790,864,2914,2145,2163,1890,3003,1995,877,1785,1489,1807,2336,1907,1693,3336,573,2193,687,2801,80,1439,2586,1613,1592,1884,910,1675,1765,1869,2006,1749,968,312,1660,1004,930,1749,2968,3116,1365,1638,1638,1638,1592,630,1560,2123,3544,505,3333,642,1792,1832,1489,1024,630,564,1536,1755,2275,3561,1560,614,950,3072,1424,2503,1489,2835,819,2560,2267,1162,552,1191,660,1117,1006,2421,592,673,2711,1438,1740,1925,2299,1436,122,1890,1966,1755,2048,2520,1365,2048,682,3072,731,1536,1137,2978,1170,2867,860,2425,2007,1905,2007,1839,1911,2002,571,2633,3117,1365,890,2048,1198,2633,1592,2730,3208,3822,2867,2730,3072,2503,3018,2048,1792,1861,1638,1755,2048,1117,341,2560,1084,2340,585,1365,2048,1638,546,1215,796,1861,3299,3803,2978,2730,2457,512,1489,1820,3072,1638,2048,3072,2849,1638,3308,1638,2048,2048,3276,3072,819,1365,1024,1365,292,2275,1137,3072,558,2925,2457,1260,1024,1365,1536,1365,1638,2275,1638,1638,240,1638,1755,2978,787,2432,1592,2792,372,3185,777,1117,463,2606,372,2835,465,3549,361,3845},
 {2048,1603,2051,1857,2914,1789,1482,2277,875,3340,1049,992,1901,1235,1698,1878,2410,2352,1704,2542,693,2481,2350,1903,2274,3068,2113,1645,1650,2018,1837,1835,1191,3267,2148,1668,2119,1735,1648,1759,1812,1813,2300,1606,2155,2291,2081,2436,2929,1195,1877,1825,2409,2136,2216,1913,1276,1028,2464,2466,2472,1831,3085,1681,956,2633,2048,1254,1445,1993,2137,1695,3102,2396,1069,2349,1394,912,717,3811,1889,885,1613,2822,1587,2089,2816,2719,2540,2251,2572,965,2419,1217,2906,2073,1725,1401,3318,1562,1881,740,2313,1089,1376,1808,2181,3004,1931,3200,2059,1121,2864,2302,1775,1471,2915,1852,1967,1609,1792,892,1778,868,1601,1289,2884,2675,902,1048,2511,1897,3009,1811,384,3724,1518,2173,2678,2546,1601,2423,1497,1861,1797,1926,1922,2308,2525,2594,1896,1807,3540,736,968,1492,3885,410,1960,3402,764,1804,1842,1554,2934,2264,2143,2290,2256,3207,2754,1932,2149,546,3055,1585,1276,2348,1241,1904,2423,2392,2732,1219,819,1683,741,1830,1920,2087,1840,2351,3200,3540,691,2697,1596,3083,2438,2503,1667,2913,2373,1280,2529,2617,3525,2098,2272,2646,2721,1011,789,2225,2416,2440,537,1728,2730,2202,2093,2250,2849,2699,2509,3645,1759,852,2788,1858,2578,2753,2137,2132,752,1238,2218,2361,1630,1554,3241,1074,883,2003,1482,2286,1410,1798,637,2212,1718,1352,2709,843,1220,2688},
 {2048,1950,1606,2178,1307,1929,2173,2054,2607,2522,2385,2155,1586,1991,1534,2049,1495,1985,2248,2331,2359,2547,1782,1912,2531,1857,2213,2248,2661,1955,2422,1662,2136,2732,774,3207,1949,2693,2026,2888,2533,1750,1301,2420,1909,2825,1905,2467,2198,1213,2982,2371,2097,2761,1187,2272,2016,904,2365,2068,1801,2384,1386,1350,1219,357,324,540,90,625,198,173,286,287,578,171,1038,344,930,353,1000,425,368,380,563,272,314,396,191,161,278,142,134,118,170,1341,892,175,2252,146,171,620,578,216,371,56,365,208,156,168,262,243,131,160,23,101,110,137,288,211,142,80,381,79,142,162,93,1230,335,1644,164,1498,182,877,103,3420,34,1157,121,276,33,1732,29,460,93,3072,337,3165,126,2586,42,744,315,3105,225,1898,367,2714,100,2678,144,1638,440,2821,101,3092,79,1832,65,3438,96,1509,54,2366,104,2983,113,2730,316,1365,257,1792,60,2275,57,2503,215,3218,139,2457,238,3486,155,1932,510,1170,916,3953,415,2560,158,1638,402,1861,275,1433,216,975,324,2821,138,1536,176,2106,27,3396,77,2363,222,1706,113,1489,76,2910,25,2891,96,1638,31,1365,332,2234,124,1489,66,1820,66,3672,106,930,49,1293,82,2275,193,3014,59,2048,23,2304,93,431,54,2184,57,1647},
 {2048,1046,393,2237,310,1869,1732,2047,69,3118,1837,1696,771,2940,1583,2141,90,2012,2048,3471,1258,1763,2287,2292,3212,1815,1928,3321,1619,1878,2855,3257,37,1954,2094,1782,2482,1406,1152,751,2816,1990,1424,2081,2900,2580,2575,2197,1668,693,2802,3105,2099,2473,2237,2359,1011,2160,2221,2232,1784,1544,1929,3332,27,1518,1852,1989,1396,1691,1003,1940,2355,1410,992,702,2808,1609,245,752,2371,419,2925,463,1043,1149,1947,2340,1798,1086,1575,2383,1992,1321,2259,3178,869,1155,3542,525,1549,1503,1958,994,1527,1387,1714,2205,1966,1297,1713,1293,1261,2126,1323,2203,2104,2635,1736,2383,1277,2218,647,1067,2142,1977,1489,3558,33,1856,1724,2048,1984,2395,1340,2993,1365,1792,2194,2867,1185,1260,1755,1724,682,2218,1298,1303,2409,2409,1911,1755,1068,3594,2048,2048,100,2275,556,1940,1613,1729,3085,2503,963,299,938,1755,1075,2194,2828,2409,1985,1774,585,2767,2218,3018,953,1820,899,1575,958,1512,1024,3018,2095,1755,2730,3103,2048,2704,318,3768,1580,2849,3098,4054,3698,1792,853,1638,803,1501,2520,2389,834,3185,2802,890,2925,3165,1732,1293,2378,1706,2275,1474,1755,1433,1495,1825,3088,2409,448,2633,1606,1489,1150,1333,843,985,1061,2802,1307,3315,303,2150,1045,1449,1764,1911,1445,2150,413,2048,656,3557,1280,2691,924,1553,574,2358,1285,4034},
 {2048,594,1583,2447,653,312,1376,3353,71,3756,569,1557,1019,2307,1942,1570,83,2279,1572,1653,3645,619,1235,1849,2314,2573,2151,2860,1864,2504,3648,3413,42,2134,1243,1696,2340,2327,1471,2064,1615,1190,558,2087,3448,1702,1857,2422,959,1201,2707,2730,2506,2269,2308,2438,1605,2114,2133,2621,2153,156,2601,3603,25,1649,534,2402,1433,1820,1852,1501,1717,1798,2457,1732,1232,1715,2114,2015,1452,1536,4011,503,163,1638,2914,2199,1977,1174,1780,1985,2389,1638,2321,2476,2307,1421,2535,1036,1560,1843,1566,1222,1061,975,1927,1072,2129,1792,1339,1724,1513,2594,3171,1241,1024,787,1462,3176,1724,1755,121,1966,1901,1706,1456,3702,17,1809,1303,2730,1007,1228,1536,2912,1365,2184,1861,1365,1536,1228,1843,2389,862,2048,1536,1077,1820,3150,768,2048,804,1861,1823,1828,2031,2000,1746,1900,805,1338,2048,2165,1609,2341,1530,1213,1733,2535,1588,3103,1536,1077,1260,2048,2304,1092,455,837,910,1170,682,1445,1755,2683,819,2168,1170,2275,2503,2123,2048,2002,1241,1592,3086,3540,3072,2145,1462,1820,890,1509,1489,1462,512,3510,2340,2560,2482,1861,1293,2168,2944,2389,1260,1170,1509,2457,910,1365,2457,3227,682,3185,341,2662,1536,983,1024,1489,1940,2340,744,1365,646,2234,1365,630,1246,2409,1146,2371,13,2802,1462,2835,1280,2048,564,3120,1638,2650,384,3844},
 {2048,1484,1057,2201,277,1523,2166,1937,195,1479,1346,2356,1316,3115,1936,2626,156,1999,2234,1730,1492,1252,2508,2185,2038,2158,1693,2475,1800,1877,2252,3517,129,1317,1694,1825,2278,2262,1014,1894,2172,2475,1293,1779,2161,2629,1622,3176,1655,2396,1330,1554,1100,2769,1699,2901,1964,1636,1888,2334,1638,2096,1031,3628,480,2157,1745,1852,1073,1851,1585,1638,1626,1734,2297,1703,1480,2048,2792,2263,1827,1988,1997,1873,690,2471,1024,1771,2415,2306,2070,1505,1908,772,2018,2846,1470,3188,2059,2340,2265,1984,970,1924,1522,1191,1655,2195,1105,2951,1984,1012,1863,2492,1844,1968,3058,2298,1717,1358,2288,1393,2247,3038,2007,2106,2353,3682,360,951,668,3081,803,1455,1568,2415,2659,1462,2997,1927,1977,1439,2048,2642,682,1609,1064,1217,1881,2004,1774,2000,2882,1321,2184,1911,1911,1981,1820,2606,2158,1310,3444,2315,1804,1996,1911,2048,2461,1547,2925,2324,953,1802,2571,2110,1985,2265,2310,1966,1655,2133,3014,2444,1412,2425,1506,2340,2616,2574,2132,3351,686,2353,2048,3086,2503,2880,2630,3220,2252,1439,2168,1664,3584,1737,2184,2150,2135,2048,1843,2891,1553,2867,2582,2163,952,2077,1847,3119,1474,1655,1482,2295,1696,1680,1409,1991,1792,2301,1618,1856,1666,3119,2118,1808,1845,1675,682,2077,1575,2147,1953,2891,1303,3174,1740,3262,1854,2489,2048,1940,1298,3425,774,3888},
 {2048,1450,1371,2072,1442,1734,2067,2282,994,3009,1268,2673,1660,2385,2244,2195,710,1692,1920,1749,1497,1856,1883,2750,2109,1779,2331,2308,1685,2147,1778,2760,1022,1597,1601,1331,1898,1527,1530,2177,2540,2598,1445,1757,2306,2238,2268,3168,1263,1784,1856,2048,2016,2375,1781,2707,1578,1600,2214,1941,1436,1668,2230,3156,1747,1091,1186,1691,884,1343,512,1114,1062,1049,1072,1041,1182,1518,1475,1432,1214,864,792,1017,1000,846,1238,637,1007,782,568,850,1038,2327,1671,3342,1189,589,2352,1248,674,561,1255,825,1094,516,802,1746,1158,1011,1725,729,877,1427,925,915,1037,1923,1604,2293,1087,1127,1103,1724,920,1063,1406,2157,2362,1574,1349,1839,912,1482,963,2048,2048,1975,945,1727,536,1890,872,2097,356,1504,819,1950,692,2248,285,1365,905,2025,1370,2348,1798,1984,1051,1804,2048,2129,1454,1483,1112,553,573,1972,170,1414,506,2678,610,1720,354,1843,484,1321,384,1849,435,768,866,2194,612,3099,797,3122,884,3155,528,3955,1193,1248,568,2371,1378,3218,1594,3177,221,819,935,1170,329,1365,573,1890,717,1365,585,1686,388,1412,646,1493,608,2152,546,1536,836,783,2666,2340,419,801,988,1917,826,2048,166,2234,317,1024,337,571,1342,1276,472,910,262,2712,826,1755,461,2473,630,1940,648,1365,1032,1998,598,1481,519,2886}
};

struct BinaryRangeEncoder {
    uint32_t x1 = 0, x2 = 0xFFFFFFFFu;
    std::vector<uint8_t>& out;
    explicit BinaryRangeEncoder(std::vector<uint8_t>& o) : out(o) {}
    void encode(int bit, int p) {                 // p = P(bit==1), 12bit (1..4095)
        if (p < 1) p = 1; else if (p > 4095) p = 4095;
        uint32_t xmid = x1 + static_cast<uint32_t>((static_cast<uint64_t>(x2 - x1) * p) >> 12);
        if (bit) x2 = xmid; else x1 = xmid + 1;
        while (((x1 ^ x2) & 0xFF000000u) == 0) { out.push_back(static_cast<uint8_t>(x2 >> 24)); x1 <<= 8; x2 = (x2 << 8) | 0xFF; }
    }
    void flush() { for (int i = 0; i < 4; ++i) { out.push_back(static_cast<uint8_t>(x1 >> 24)); x1 <<= 8; } }
};
struct BinaryRangeDecoder {
    uint32_t x1 = 0, x2 = 0xFFFFFFFFu, x = 0;
    const uint8_t* in; size_t pos = 0, size;
    BinaryRangeDecoder(const uint8_t* d, size_t s) : in(d), size(s) { for (int i = 0; i < 4; ++i) x = (x << 8) | rb(); }
    uint8_t rb() { return pos < size ? in[pos++] : 0; }
    int decode(int p) {
        if (p < 1) p = 1; else if (p > 4095) p = 4095;
        uint32_t xmid = x1 + static_cast<uint32_t>((static_cast<uint64_t>(x2 - x1) * p) >> 12);
        int bit = (x <= xmid) ? 1 : 0;
        if (bit) x2 = xmid; else x1 = xmid + 1;
        while (((x1 ^ x2) & 0xFF000000u) == 0) { x1 <<= 8; x2 = (x2 << 8) | 0xFF; x = (x << 8) | rb(); }
        return bit;
    }
};

// 適応カウンタの学習レート (16.16 固定小数, 1/(n+α)相当)。ファイル種別ごとにプロファイルを選ぶ:
//   SLOW = 低い床(~1/16): 定常的なテキスト/画像向け (CM, BMP_CM)。
//   FAST = 高い床(~1/5):  非定常な exe/音声残差向け (BCJ_CM, WAV_CM)。
// algo バイトはアーカイブに保存され復号も同じ algo を見るため、プロファイル選択は完全可逆。

// CM 予測モデル (encode/decode 共通)
//   文脈モデル: order 0,1,2,3,4,5,6 + マッチ = 8 入力。mixer + APM(二次推定)。
//   各テーブル要素 uint16 = (prob<<4)|count : prob は 12bit, count(0..15) で学習率を制御。
struct CMModel {
    static const int NIN = 15;                     // o0..o8,stride3,match x3,x86 operand,SJIS text
    const int TBITS, TSIZE, TMASK;                  // t2..t9 のサイズ (プロファイル依存)
    static const int SM = 1 << 24;
    static const int EXE_BITS = 22, EXE_SIZE = 1 << EXE_BITS, EXE_MASK = EXE_SIZE - 1;
    static const int TEXT_BITS = 22, TEXT_SIZE = 1 << TEXT_BITS, TEXT_MASK = TEXT_SIZE - 1;
    std::vector<uint16_t> t0, t1, t2, t3, t4, t5, t6, t7, t8, t9;  // ビット確率 (12bit, 初期 2048)
    std::vector<uint16_t> tExe;                    // x86 opcode + operand byte position (FAST専用)
    std::vector<uint16_t> tText;                   // Shift-JIS構造・文字クラス文脈 (SLOW専用)
    std::vector<uint32_t> matchTab, matchTab2, matchTab3;
    std::vector<uint8_t> buf;
    std::vector<int> w;                            // mixer 重み (mixCtx 文脈 x NIN)
    std::vector<int> w2;                           // 第2 mixer 重み (order-2 文脈 x NIN)
    std::vector<int> w3;                           // 第3 mixer 重み (order-3 文脈 x NIN)
    std::vector<int> w4;                           // 第4 mixer 重み (order-4 文脈 x NIN)
    std::vector<int> wf;                           // 最終 mixer (sub-mixer をbitpos毎に学習合成)
    static const int NMIX = 4;
    int mix2Ctx = 0, mix3Ctx = 0, mix4Ctx = 0, fmCtx = 0, fmLogit[NMIX] = {0,0,0,0};
    std::vector<uint16_t> apm;                     // 一次推定 (8192 文脈 x 65 点、match強度付き)
    std::vector<uint16_t> apm2;                    // 二次推定 (2048 文脈 x 65 点、bitpos付き)
    std::vector<uint16_t> apm3;                    // 三次推定 (1024 文脈 x 65 点、c0×match強度)
    std::vector<uint16_t> apm4;                    // 四次推定 (2048 文脈 x 65 点、cx[3]ハッシュ+bitpos)
    uint32_t matchPtr = 0; int matchLen = 0;
    uint32_t matchPtr2 = 0; int matchLen2 = 0;     // 第2マッチモデル (6バイトハッシュ)
    uint32_t matchPtr3 = 0; int matchLen3 = 0;     // 第3マッチモデル (8バイトハッシュ)
    uint32_t cx[9] = {0,0,0,0,0,0,0,0,0};           // cx[k] = 直近 k バイトのハッシュ
    bool isExe = false, exeActive = false;
    int exeRemain = 0, exeClass = 0, exeOpcode = 0;
    bool isText = false, sjisTrail = false;
    int sjisLead = 0, textIdx = 0;
    uint16_t textPrevChar = 0;
    uint32_t textClasses = 0;                      // 直近6トークンの4bit文字クラス
    int c0 = 1, bitpos = 0, mc = 0, mc_ext = 0;    // mc_ext = mc*8+bitpos (APM1用)
    int mixCtx = 0;                                // ミキサー文脈 = mc_ext*2 + match-active
    int ms_apm = 0;                                // 8段階 match strength (APM3専用)
    int ms_apm16 = 0;                              // 16段階 match strength (APM1専用)
    int st[NIN], idx[NIN], pr0 = 2048, prf = 2048, apmIdx = 0;
    int apm2Ctx = 0, apm2Idx = 0, apm2Wt = 0;
    int apm3Idx = 0, apm3Wt = 0;
    int apm4Ctx = 0, apm4Idx = 0, apm4Wt = 0;
    const int* rate = CM_RATE_SLOW;                // 適応カウンタ学習レートのプロファイル
    int mixShift = 12;                             // ミキサー学習レート (プロファイル依存)
    int apmShift = 7;                              // APM 更新レート (プロファイル依存)
    int subShift = 24;                             // sub-mixer 文脈の細かさ (プロファイル依存)
    int strideLen = 3;                             // スパース文脈の刻み (プロファイル依存)

    CMModel(const CMProfile& prof)
              : TBITS(prof.tbits), TSIZE(1 << prof.tbits), TMASK((1 << prof.tbits) - 1),
                t0(512, 32768), t1(256 * 512, 32768), t2(TSIZE, 32768), t3(TSIZE, 32768),
                t4(TSIZE, 32768), t5(TSIZE, 32768), t6(TSIZE, 32768), t7(TSIZE, 32768),
                t8(TSIZE, 32768), t9(TSIZE, 32768), tExe(prof.tbits == 29 ? EXE_SIZE : 1, 32768),
                tText((prof.tbits == 27 && prof.mixShift == 11 && prof.apmShift == 8 && prof.strideLen == 2) ? TEXT_SIZE : 1, 32768),
                matchTab(SM, 0), matchTab2(SM, 0), matchTab3(SM, 0), w(8192 * NIN, 1 << 14), w2(2097152 * NIN, 1 << 14), w3(2097152 * NIN, 1 << 14), w4(2097152 * NIN, 1 << 14), wf(64 * NMIX, 16384),
                apm(32768 * 65), apm2(4096 * 65), apm3(32768 * 65), apm4(524288 * 65) {
        rate = prof.rate; mixShift = prof.mixShift; apmShift = prof.apmShift; subShift = prof.subShift; strideLen = prof.strideLen;
        isExe = prof.tbits == 29;
        isText = prof.tbits == 27 && prof.mixShift == 11 && prof.apmShift == 8 && prof.strideLen == 2;
        if (isExe) {
            // coarse priorを、既存のopcode/position/hash表へ展開する。局所学習は通常どおり継続。
            for (int cls = 1; cls <= 2; ++cls) {
                int opFirst = cls == 1 ? 0xE8 : 0xB8;
                int opLast  = cls == 1 ? 0xE9 : 0xBF;
                for (int remain = 1; remain <= 4; ++remain) {
                    int group = (cls - 1) * 4 + (remain - 1);
                    for (int opcode = opFirst; opcode <= opLast; ++opcode) {
                        for (int pos16 = 0; pos16 < 16; ++pos16) {
                            for (int prefix = 1; prefix < 256; ++prefix) {
                                uint32_t eh = static_cast<uint32_t>(prefix);
                                eh = eh * 0x9E3779B1u + static_cast<uint32_t>(opcode + 1);
                                eh = eh * 0x9E3779B1u + static_cast<uint32_t>((cls << 3) | remain);
                                eh = eh * 0x9E3779B1u + static_cast<uint32_t>(pos16);
                                int ix = static_cast<int>(eh & EXE_MASK);
                                tExe[ix] = static_cast<uint16_t>((EXE_PRIOR[group][prefix] << 4) | 15);
                            }
                        }
                    }
                }
            }
        }
        uint16_t initv[65];
        for (int j = 0; j < 65; ++j) initv[j] = static_cast<uint16_t>(CM_squash((j - 32) * 64) * 16);
        for (int i = 0; i < 32768; ++i)
            for (int j = 0; j < 65; ++j) apm[i * 65 + j] = initv[j];
        for (int i = 0; i < 4096; ++i)
            for (int j = 0; j < 65; ++j) apm2[i * 65 + j] = initv[j];
        for (int i = 0; i < 524288; ++i)
            for (int j = 0; j < 65; ++j) apm4[i * 65 + j] = initv[j];
        for (int i = 0; i < 16384; ++i)
            for (int j = 0; j < 65; ++j)
                apm3[i * 65 + j] = initv[j];
    }

    int predict() {
        idx[0] = c0;                                                       // order0
        idx[1] = static_cast<int>((cx[1] & 0xFF) * 512 + c0);             // order1
        idx[2] = static_cast<int>(((cx[2] * 0x9E3779B1u) + c0) & TMASK);  // order2
        idx[3] = static_cast<int>(((cx[3] * 0x9E3779B1u) + c0) & TMASK);  // order3
        idx[4] = static_cast<int>(((cx[4] * 0x9E3779B1u) + c0) & TMASK);  // order4
        idx[5] = static_cast<int>(((cx[5] * 0x9E3779B1u) + c0) & TMASK);  // order5
        idx[6] = static_cast<int>(((cx[6] * 0x9E3779B1u) + c0) & TMASK);  // order6
        idx[7] = static_cast<int>(((cx[7] * 0x9E3779B1u) + c0) & TMASK);  // order7
        idx[8] = static_cast<int>(((cx[8] * 0x9E3779B1u) + c0) & TMASK);  // order8
        // スパース文脈: -s, -2s, -3s バイト (s=strideLen; txt=3 UTF-8整列, exe=4 dword整列)
        {
            size_t p = buf.size();
            int s = strideLen;
            uint32_t sh3 = static_cast<uint32_t>(c0);
            if (p >= static_cast<size_t>(s))     sh3 = sh3 * 0x9E3779B1u + buf[p - s] + 1u;
            if (p >= static_cast<size_t>(2 * s)) sh3 = sh3 * 0x9E3779B1u + buf[p - 2 * s] + 1u;
            if (p >= static_cast<size_t>(3 * s)) sh3 = sh3 * 0x9E3779B1u + buf[p - 3 * s] + 1u;
            idx[9] = static_cast<int>(sh3 & TMASK);
        }
        st[0] = CM_STR.v[t0[idx[0]] >> 4];
        st[1] = CM_STR.v[t1[idx[1]] >> 4];
        st[2] = CM_STR.v[t2[idx[2]] >> 4];
        st[3] = CM_STR.v[t3[idx[3]] >> 4];
        st[4] = CM_STR.v[t4[idx[4]] >> 4];
        st[5] = CM_STR.v[t5[idx[5]] >> 4];
        st[6] = CM_STR.v[t6[idx[6]] >> 4];
        st[7] = CM_STR.v[t7[idx[7]] >> 4];
        st[8] = CM_STR.v[t8[idx[8]] >> 4];
        st[9] = CM_STR.v[t9[idx[9]] >> 4];
        st[10] = 0;                                 // match model
        if (matchPtr > 0 && matchPtr < buf.size()) {
            int predByte = buf[matchPtr];
            int bitsSoFar = c0 - (1 << bitpos);
            int expected = predByte >> (8 - bitpos);
            if (bitsSoFar == expected) {
                int predBit = (predByte >> (7 - bitpos)) & 1;
                int conf = (matchLen < 28 ? matchLen : 28) * 72;
                st[10] = predBit ? conf : -conf;
            }
        }
        st[11] = 0;                                 // 第2マッチモデル (6バイトハッシュ)
        if (matchPtr2 > 0 && matchPtr2 < buf.size()) {
            int predByte = buf[matchPtr2];
            int bitsSoFar = c0 - (1 << bitpos);
            int expected = predByte >> (8 - bitpos);
            if (bitsSoFar == expected) {
                int predBit = (predByte >> (7 - bitpos)) & 1;
                int conf = (matchLen2 < 28 ? matchLen2 : 28) * 72;
                st[11] = predBit ? conf : -conf;
            }
        }
        st[12] = 0;                                 // 第3マッチモデル (8バイトハッシュ)
        if (matchPtr3 > 0 && matchPtr3 < buf.size()) {
            int predByte = buf[matchPtr3];
            int bitsSoFar = c0 - (1 << bitpos);
            int expected = predByte >> (8 - bitpos);
            if (bitsSoFar == expected) {
                int predBit = (predByte >> (7 - bitpos)) & 1;
                int conf = (matchLen3 < 28 ? matchLen3 : 28) * 72;
                st[12] = predBit ? conf : -conf;
            }
        }
        // x86 operand model: opcode と immediate/relative operand 内のバイト位置を共有文脈化。
        // BCJ後のrel32は各バイト位置で分布が大きく異なるため、通常のbyte-order文脈と分離する。
        st[13] = 0;
        exeActive = isExe && exeRemain > 0;
        if (exeActive) {
            uint32_t eh = static_cast<uint32_t>(c0);
            eh = eh * 0x9E3779B1u + static_cast<uint32_t>(exeOpcode + 1);
            eh = eh * 0x9E3779B1u + static_cast<uint32_t>((exeClass << 3) | exeRemain);
            eh = eh * 0x9E3779B1u + static_cast<uint32_t>(buf.size() & 15);
            idx[13] = static_cast<int>(eh & EXE_MASK);
            st[13] = CM_STR.v[tExe[idx[13]] >> 4];
        }
        // Shift-JIS text model: byte-order文脈とは別に文字境界と粗い文字種を共有する。
        st[14] = 0;
        if (isText) {
            uint32_t th = static_cast<uint32_t>(c0);
            th = th * 0x9E3779B1u + textClasses;
            th = th * 0x9E3779B1u + static_cast<uint32_t>(textPrevChar + 1);
            th = th * 0x9E3779B1u + static_cast<uint32_t>(sjisTrail ? (0x100 | sjisLead) : 0);
            textIdx = static_cast<int>(th & TEXT_MASK);
            st[14] = CM_STR.v[tText[textIdx] >> 4];
        }
        mc = static_cast<int>(cx[1] & 0xFF);
        mc_ext = mc * 8 + bitpos;
        int ms = matchLen == 0 ? 0 : (matchLen < 8 ? 1 : (matchLen < 32 ? 2 : 3));
        ms_apm = matchLen == 0 ? 0 : (matchLen < 4 ? 1 : (matchLen < 8 ? 2 : (matchLen < 16 ? 3 : (matchLen < 32 ? 4 : (matchLen < 64 ? 5 : (matchLen < 128 ? 6 : 7))))));  // 8段階 (APM3用)
        ms_apm16 = matchLen == 0 ? 0 : (matchLen < 2 ? 1 : (matchLen < 3 ? 2 : (matchLen < 4 ? 3 : (matchLen < 6 ? 4 : (matchLen < 8 ? 5 : (matchLen < 12 ? 6 : (matchLen < 16 ? 7 : (matchLen < 24 ? 8 : (matchLen < 32 ? 9 : (matchLen < 48 ? 10 : (matchLen < 64 ? 11 : (matchLen < 96 ? 12 : (matchLen < 128 ? 13 : (matchLen < 192 ? 14 : 15))))))))))))));  // 16段階 (APM1用)
        mixCtx = mc_ext * 4 + ms;                       // match 強度 (2bit) で別重み集合
        mix2Ctx = static_cast<int>(((cx[2] * 0x9E3779B1u) >> subShift) * 8 + bitpos);  // order-2 文脈
        mix3Ctx = static_cast<int>(((cx[3] * 0x9E3779B1u) >> subShift) * 8 + bitpos);  // order-3 文脈
        mix4Ctx = static_cast<int>(((cx[4] * 0x9E3779B1u) >> subShift) * 8 + bitpos);  // order-4 文脈
        long long dot = 0, dot2 = 0, dot3 = 0, dot4 = 0;
        for (int i = 0; i < NIN; ++i) {
            dot  += static_cast<long long>(w [mixCtx  * NIN + i]) * st[i];
            dot2 += static_cast<long long>(w2[mix2Ctx * NIN + i]) * st[i];
            dot3 += static_cast<long long>(w3[mix3Ctx * NIN + i]) * st[i];
            dot4 += static_cast<long long>(w4[mix4Ctx * NIN + i]) * st[i];
        }
        // sub-mixer 出力を最終 mixer が bitpos 毎に学習合成 (2層 mixer)
        fmLogit[0] = static_cast<int>(dot >> 16);
        fmLogit[1] = static_cast<int>(dot2 >> 16);
        fmLogit[2] = static_cast<int>(dot3 >> 16);
        fmLogit[3] = static_cast<int>(dot4 >> 16);
        fmCtx = bitpos * 8 + ms_apm;                    // bitpos + match強度8段階 で sub-mixer 配分を変える
        long long dotF = 0;
        for (int k = 0; k < NMIX; ++k) dotF += static_cast<long long>(wf[fmCtx * NMIX + k]) * fmLogit[k];
        pr0 = CM_squash(static_cast<int>(dotF >> 16));
        if (pr0 < 1) pr0 = 1; else if (pr0 > 4094) pr0 = 4094;
        // APM1: mixer 出力を文脈 (直前バイト*8+ビット位置+match強度8段階) で補正 (65点補間)
        int s = CM_STR.v[pr0] + 2048;               // 0..4095
        int wt = s & 63, j = s >> 6;
        apmIdx = (mc_ext * 16 + ms_apm16) * 65 + j;  // 32768文脈 (mc_ext=2048, ms_apm16=16)
        int ap = (apm[apmIdx] * (64 - wt) + apm[apmIdx + 1] * wt) >> 10;    // 12bit
        prf = (pr0 + 3 * ap) >> 2;
        if (prf < 1) prf = 1; else if (prf > 4094) prf = 4094;
        // APM2: prf を 2次文脈 (cx[2]のハッシュ上位9bit+bitpos) でさらに補正 (65点補間)
        apm2Ctx = static_cast<int>(((cx[2] * 0x9E3779B1u) >> 23) * 8 + bitpos);  // 4096文脈 (9bit+3bit)
        int s2 = CM_STR.v[prf] + 2048;
        apm2Wt = s2 & 63; int j2 = s2 >> 6;
        apm2Idx = apm2Ctx * 65 + j2;
        int ap2 = (apm2[apm2Idx] * (64 - apm2Wt) + apm2[apm2Idx + 1] * apm2Wt) >> 10;
        prf = (prf + ap2) >> 1;
        if (prf < 1) prf = 1; else if (prf > 4094) prf = 4094;
        // APM3: prf を c0 (バイト内部分ビット列) でさらに補正 (65点補間)
        {
            int s3 = CM_STR.v[prf] + 2048;
            apm3Wt = s3 & 63; int j3 = s3 >> 6;
            apm3Idx = (((mc >> 5) & 7) * 2048 + c0 * 8 + ms_apm) * 65 + j3;  // prev3bits*2048+c0*8+ms_apm → 16384文脈
            int ap3 = (apm3[apm3Idx] * (64 - apm3Wt) + apm3[apm3Idx + 1] * apm3Wt) >> 10;
            prf = (prf + ap3) >> 1;
            if (prf < 1) prf = 1; else if (prf > 4094) prf = 4094;
        }
        // APM4: prf を cx[4]ハッシュ上位15bit+match有無+bitpos でさらに補正 (65点補間)
        apm4Ctx = static_cast<int>(((cx[4] * 0x9E3779B1u) >> 17) * 16 + (matchLen > 0 ? 8 : 0) + bitpos);  // 524288文脈
        {
            int s4 = CM_STR.v[prf] + 2048;
            apm4Wt = s4 & 63; int j4 = s4 >> 6;
            apm4Idx = apm4Ctx * 65 + j4;
            int ap4 = (apm4[apm4Idx] * (64 - apm4Wt) + apm4[apm4Idx + 1] * apm4Wt) >> 10;
            prf = (prf + ap4) >> 1;
            if (prf < 1) prf = 1; else if (prf > 4094) prf = 4094;
        }
        return prf;
    }
    void update(int bit) {
        int err = (bit << 12) - pr0;                // 両 mixer は最終出力誤差で学習
        for (int i = 0; i < NIN; ++i) {
            int& wi = w[mixCtx * NIN + i];
            wi += (st[i] * err) >> mixShift;
            if (wi < -(1 << 20)) wi = -(1 << 20); else if (wi > (1 << 20)) wi = (1 << 20);
            int& wi2 = w2[mix2Ctx * NIN + i];
            wi2 += (st[i] * err) >> mixShift;
            if (wi2 < -(1 << 20)) wi2 = -(1 << 20); else if (wi2 > (1 << 20)) wi2 = (1 << 20);
            int& wi3 = w3[mix3Ctx * NIN + i];
            wi3 += (st[i] * err) >> mixShift;
            if (wi3 < -(1 << 20)) wi3 = -(1 << 20); else if (wi3 > (1 << 20)) wi3 = (1 << 20);
            int& wi4 = w4[mix4Ctx * NIN + i];
            wi4 += (st[i] * err) >> mixShift;
            if (wi4 < -(1 << 20)) wi4 = -(1 << 20); else if (wi4 > (1 << 20)) wi4 = (1 << 20);
        }
        for (int k = 0; k < NMIX; ++k) {            // 最終 mixer 更新
            int& wfk = wf[fmCtx * NMIX + k];
            wfk += (fmLogit[k] * err) >> 14;
            if (wfk < -(1 << 18)) wfk = -(1 << 18); else if (wfk > (1 << 18)) wfk = (1 << 18);
        }
        int g = bit << 16;                          // APM1 更新
        apm[apmIdx]     = static_cast<uint16_t>(apm[apmIdx]     + ((g - apm[apmIdx])     >> apmShift));
        apm[apmIdx + 1] = static_cast<uint16_t>(apm[apmIdx + 1] + ((g - apm[apmIdx + 1]) >> apmShift));
        // APM2 更新
        apm2[apm2Idx]     = static_cast<uint16_t>(apm2[apm2Idx]     + ((g - apm2[apm2Idx])     >> apmShift));
        apm2[apm2Idx + 1] = static_cast<uint16_t>(apm2[apm2Idx + 1] + ((g - apm2[apm2Idx + 1]) >> apmShift));
        // APM3 更新
        apm3[apm3Idx]     = static_cast<uint16_t>(apm3[apm3Idx]     + ((g - apm3[apm3Idx])     >> apmShift));
        apm3[apm3Idx + 1] = static_cast<uint16_t>(apm3[apm3Idx + 1] + ((g - apm3[apm3Idx + 1]) >> apmShift));
        // APM4 更新
        apm4[apm4Idx]     = static_cast<uint16_t>(apm4[apm4Idx]     + ((g - apm4[apm4Idx])     >> apmShift));
        apm4[apm4Idx + 1] = static_cast<uint16_t>(apm4[apm4Idx + 1] + ((g - apm4[apm4Idx + 1]) >> apmShift));
        int tgt = bit << 12;
        auto upd  = [&](std::vector<uint16_t>& t, int ix) {
            uint16_t e = t[ix];
            int n = e & 15, pr = e >> 4;
            pr += (((tgt - pr) * rate[n]) >> 16);
            if (pr < 0) pr = 0; else if (pr > 4095) pr = 4095;
            if (n < 15) ++n;
            t[ix] = static_cast<uint16_t>((pr << 4) | n);
        };
        upd(t0, idx[0]); upd(t1, idx[1]); upd(t2, idx[2]); upd(t3, idx[3]);
        upd(t4, idx[4]); upd(t5, idx[5]); upd(t6, idx[6]); upd(t7, idx[7]); upd(t8, idx[8]); upd(t9, idx[9]);
        if (exeActive) upd(tExe, idx[13]);
        if (isText) upd(tText, textIdx);
        c0 = (c0 << 1) | bit; ++bitpos;
        if (bitpos == 8) {
            int B = c0 & 0xFF;
            buf.push_back(static_cast<uint8_t>(B));
            if (isExe) {
                if (exeRemain > 0) {
                    --exeRemain;
                    if (exeRemain == 0) { exeClass = 0; exeOpcode = 0; }
                } else if (B == 0xE8 || B == 0xE9) {
                    exeClass = 1; exeOpcode = B; exeRemain = 4;          // CALL/JMP rel32 (BCJ対象)
                } else if (B >= 0xB8 && B <= 0xBF) {
                    exeClass = 2; exeOpcode = B; exeRemain = 4;          // MOV reg, imm32
                }
            }
            if (isText) {
                auto isLead = [](int x) { return (x >= 0x81 && x <= 0x9F) || (x >= 0xE0 && x <= 0xFC); };
                int cls = 0;
                if (sjisTrail) {
                    uint16_t ch = static_cast<uint16_t>((sjisLead << 8) | B);
                    if (sjisLead == 0x82 && B >= 0x9F && B <= 0xF1) cls = 6;      // ひらがな
                    else if (sjisLead == 0x83) cls = 7;                           // カタカナ
                    else if (sjisLead == 0x81) cls = 8;                           // 全角記号
                    else cls = 9;                                                 // 漢字ほか
                    textPrevChar = ch;
                    sjisTrail = false; sjisLead = 0;
                } else if (isLead(B)) {
                    sjisLead = B; sjisTrail = true;
                } else {
                    if (B == '\r' || B == '\n') cls = 1;
                    else if (B == ' ' || B == '\t') cls = 2;
                    else if (B >= '0' && B <= '9') cls = 3;
                    else if ((B >= 'A' && B <= 'Z') || (B >= 'a' && B <= 'z')) cls = 4;
                    else cls = 5;
                    textPrevChar = static_cast<uint16_t>(B);
                }
                if (cls != 0) textClasses = ((textClasses << 4) | static_cast<uint32_t>(cls)) & 0xFFFFFFu;
            }
            if (matchPtr > 0 && matchPtr < buf.size() - 1 && buf[matchPtr] == B) { ++matchPtr; ++matchLen; }
            else { matchPtr = 0; matchLen = 0; }
            if (matchPtr2 > 0 && matchPtr2 < buf.size() - 1 && buf[matchPtr2] == B) { ++matchPtr2; ++matchLen2; }
            else { matchPtr2 = 0; matchLen2 = 0; }
            if (matchPtr3 > 0 && matchPtr3 < buf.size() - 1 && buf[matchPtr3] == B) { ++matchPtr3; ++matchLen3; }
            else { matchPtr3 = 0; matchLen3 = 0; }
            size_t p = buf.size();
            uint32_t hsh = 0;                        // cx[k] = 直近 k バイトの累積ハッシュ
            for (int k = 1; k <= 8; ++k) { if (p >= static_cast<size_t>(k)) hsh = hsh * 0x9E3779B1u + buf[p - k] + 1u; cx[k] = hsh; }
            if (p >= 4) {
                uint32_t hh = (static_cast<uint32_t>(buf[p - 1]) | (static_cast<uint32_t>(buf[p - 2]) << 8)
                             | (static_cast<uint32_t>(buf[p - 3]) << 16) | (static_cast<uint32_t>(buf[p - 4]) << 24));
                hh = (hh * 2654435761u) & (SM - 1);
                if (matchPtr == 0) { uint32_t cand = matchTab[hh]; if (cand > 0 && cand < p) { matchPtr = cand; matchLen = 1; } }
                matchTab[hh] = static_cast<uint32_t>(p);
            }
            if (p >= 6) {                            // 第2マッチ: 直近6バイトハッシュ
                uint32_t h2 = 0;
                for (int k = 1; k <= 6; ++k) h2 = h2 * 0x9E3779B1u + buf[p - k] + 1u;
                h2 = (h2 * 2654435761u) & (SM - 1);
                if (matchPtr2 == 0) { uint32_t cand = matchTab2[h2]; if (cand > 0 && cand < p) { matchPtr2 = cand; matchLen2 = 1; } }
                matchTab2[h2] = static_cast<uint32_t>(p);
            }
            if (p >= 8) {                            // 第3マッチ: 直近8バイトハッシュ
                uint32_t h3 = 0;
                for (int k = 1; k <= 8; ++k) h3 = h3 * 0x9E3779B1u + buf[p - k] + 1u;
                h3 = (h3 * 2654435761u) & (SM - 1);
                if (matchPtr3 == 0) { uint32_t cand = matchTab3[h3]; if (cand > 0 && cand < p) { matchPtr3 = cand; matchLen3 = 1; } }
                matchTab3[h3] = static_cast<uint32_t>(p);
            }
            c0 = 1; bitpos = 0;
        }
    }
};

std::vector<uint8_t> Encode_CM(const std::vector<uint8_t>& input, const CMProfile& prof) {
    std::vector<uint8_t> out;
    PutU64(out, static_cast<uint64_t>(input.size()));
    if (input.empty()) return out;
    CMModel cm(prof);
    BinaryRangeEncoder enc(out);
    for (uint8_t B : input) {
        for (int k = 7; k >= 0; --k) {
            int bit = (B >> k) & 1;
            int p = cm.predict();
            enc.encode(bit, p);
            cm.update(bit);
        }
    }
    enc.flush();
    return out;
}
std::vector<uint8_t> Decode_CM(const std::vector<uint8_t>& input, const CMProfile& prof) {
    if (input.size() < 8) return {};
    uint64_t n = GetU64(input.data());
    std::vector<uint8_t> out;
    if (n == 0) return out;
    out.reserve(static_cast<size_t>(n));
    CMModel cm(prof);
    BinaryRangeDecoder dec(input.data() + 8, input.size() - 8);
    for (uint64_t i = 0; i < n; ++i) {
        int B = 0;
        for (int k = 0; k < 8; ++k) {
            int p = cm.predict();
            int bit = dec.decode(p);
            cm.update(bit);
            B = (B << 1) | bit;
        }
        out.push_back(static_cast<uint8_t>(B));
    }
    return out;
}
