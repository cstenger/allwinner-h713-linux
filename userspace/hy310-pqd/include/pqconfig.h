/*
 * hy310-pqd — Configuration parser for stock tvconfig/*.{db,ini,xml}.
 *
 * Loads:
 *   tvpq.db                         (SQLite) — Picture_Mode, White_Balance_Mode, Gamma_Point
 *   pqcontrol_custom_setting.xml    — user's current slider values
 *   pqcontrol_config_setting.xml    — per-mode defaults
 *   pq_picturemode.ini              — picture-mode presets (Vivid/Cinema/Game/…)
 *   pq_colortemp.ini                — colour-temp gains/offsets
 *
 * We mirror Stock's data model so the same XML/DB files can be dropped
 * into /etc/hy310/ on a mainline system without modification.
 *
 * Source-of-truth: /opt/hy310/stock-re/super_extracted/vendor/etc/tvconfig/
 */
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace hy310 {
namespace pqconfig {

/* ---- enums matching Stock ---------------------------------------- */

enum class SourceType : int {    /* tvin column in tvpq.db */
    VIDEODEC = 0,
    HDMI1    = 1,
    HDMI2    = 2,
    CVBS     = 3,
    ATV      = 4,
    DTV      = 5,
};

enum class PictureMode : int {   /* mode column in tvpq.db Picture_Mode */
    STANDARD = 0,
    CINEMA   = 1,
    VIVID    = 2,
    GAME     = 3,
    COMPUTER = 4,
    HDR      = 5,
    CUSTOM   = 6,
};

/* ---- records from tvpq.db --------------------------------------- */

struct PictureModeRow {
    int tvin;
    int mode;
    int brightness;        /* 0..100 */
    int contrast;          /* 0..100 */
    int saturation;        /* 0..100 */
    int hue;               /* 0..100 */
    int sharpness;         /* 0..100 */
    int tnr;               /* 0..3  */
    int snr;               /* 0..3  */
    int backlight;         /* 0..100 */
    int colortemperature;  /* index 0..3 (STANDARD/COOL/WARM/USER) */
    int gamma;             /* index 0..n (the gamma factor we saw = 3) */
    int dci;               /* 0..3  */
    int blackextension;    /* 0..3  */
    int dynamic_backlight; /* 0..1  */
    std::string name;
};

struct WhiteBalanceRow {
    int tvin;
    int mode;
    int rgain, ggain, bgain;        /* 0..1023 typically */
    int roffset, goffset, boffset;  /* -512..511 */
};

/* 33 control points for a gamma curve.  Stock's Gamma_Point table has 33
 * rows with values 0..4095 (12-bit); the defaults ship as all-zero and
 * get overwritten by Factory calibration or user override. */
using GammaPoints = std::array<int16_t, 33>;

/* ---- XML-provided current state --------------------------------- */

struct CustomSetting {                /* from pqcontrol_custom_setting.xml */
    int current_source_type   = 0;
    struct Sliders {
        int cvbs_pedestal_mode = 1;
        int brightness         = 50;
        int contrast           = 50;
        int saturation         = 50;
        int hue                = 50;
        int sharpness          = 50;
        int backlight          = 100;
        int colortemperature   = 0;
        int gamma              = 3;
        int tnr                = 2;
        int snr                = 1;
        int dci                = 2;
        int blackextension     = 1;
        int dynamic_backlight  = 0;
        int device_mode        = 0;
    };
    Sliders current_data;
    Sliders custom_videodec;
    Sliders custom_hdmi1;
    Sliders custom_hdmi2;
    Sliders custom_cvbs;
    std::string mode_videodec = "standard";
    std::string mode_hdmi1    = "standard";
    std::string mode_hdmi2    = "standard";
    std::string mode_cvbs     = "standard";
};

/* ---- color-temp from pq_colortemp.ini --------------------------- */

struct ColorTempEntry {
    std::string mode_name;           /* STANDARD/COOL/WARM/USER */
    int roffset, goffset, boffset;
    int rgain, ggain, bgain;
};
using ColorTempSet = std::map<SourceType, std::vector<ColorTempEntry>>;

/* ---- top-level aggregate ---------------------------------------- */

struct Config {
    std::vector<PictureModeRow>  picture_modes;   /* ≤ 25 rows in stock */
    std::vector<WhiteBalanceRow> white_balance;   /* ≤ 20 rows in stock */
    GammaPoints                  gamma_points{};  /* 33 rows from Gamma_Point */
    CustomSetting                custom;          /* from XML */
    ColorTempSet                 color_temp;      /* from ini */

    /* Helper: pick the right PictureModeRow for a given (tvin, mode). */
    std::optional<PictureModeRow> find_mode(int tvin, int mode) const;
    /* Helper: same for white balance. */
    std::optional<WhiteBalanceRow> find_wb(int tvin, int mode) const;
};

/* ---- loaders ---------------------------------------------------- */

/* Load tvpq.db (requires libsqlite3). */
int load_tvpq_db(const std::string& path, Config& out);

/* Load pqcontrol_custom_setting.xml (libexpat-based inline parser — no
 * external XML dep, supports only the schema we see in stock). */
int load_custom_setting_xml(const std::string& path, Config& out);

/* Load pq_colortemp.ini.  Tiny INI parser, no external dep. */
int load_colortemp_ini(const std::string& path, Config& out);

/* Load all known config files from a base directory (typically
 * /etc/hy310/tvconfig/). Missing files are not fatal — they just leave
 * the corresponding part of Config at defaults. */
int load_all(const std::string& base_dir, Config& out);

} // namespace pqconfig
} // namespace hy310
