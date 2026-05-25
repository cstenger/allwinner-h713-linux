/*
 * Config parser implementation.
 */
#include "pqconfig.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <regex>
#include <sqlite3.h>

namespace hy310 {
namespace pqconfig {

/* --------------------------------------------------------------- *
 * Config helpers
 * --------------------------------------------------------------- */

std::optional<PictureModeRow> Config::find_mode(int tvin, int mode) const {
    for (const auto& r : picture_modes)
        if (r.tvin == tvin && r.mode == mode) return r;
    return std::nullopt;
}

std::optional<WhiteBalanceRow> Config::find_wb(int tvin, int mode) const {
    for (const auto& r : white_balance)
        if (r.tvin == tvin && r.mode == mode) return r;
    return std::nullopt;
}

/* --------------------------------------------------------------- *
 * tvpq.db — SQLite
 * --------------------------------------------------------------- */

namespace {

int pm_callback(void* out_ptr, int cols, char** vals, char** names) {
    auto* vec = static_cast<std::vector<PictureModeRow>*>(out_ptr);
    PictureModeRow r{};
    auto as_int = [&](const char* v) { return v ? std::atoi(v) : 0; };
    auto as_str = [&](const char* v) { return v ? std::string(v) : std::string(); };

    for (int i = 0; i < cols; ++i) {
        std::string n = names[i] ? names[i] : "";
        const char* v = vals[i];
        if      (n == "tvin")             r.tvin = as_int(v);
        else if (n == "mode")             r.mode = as_int(v);
        else if (n == "brightness")       r.brightness = as_int(v);
        else if (n == "contrast")         r.contrast = as_int(v);
        else if (n == "saturation")       r.saturation = as_int(v);
        else if (n == "hue")              r.hue = as_int(v);
        else if (n == "sharpness")        r.sharpness = as_int(v);
        else if (n == "tnr")              r.tnr = as_int(v);
        else if (n == "snr")              r.snr = as_int(v);
        else if (n == "backlight")        r.backlight = as_int(v);
        else if (n == "colortemperature") r.colortemperature = as_int(v);
        else if (n == "gamma")            r.gamma = as_int(v);
        else if (n == "dci")              r.dci = as_int(v);
        /* Stock has a typo in the column name — "blackextenstion" */
        else if (n == "blackextenstion")  r.blackextension = as_int(v);
        else if (n == "blackextension")   r.blackextension = as_int(v);
        else if (n == "dynamic_backlight") r.dynamic_backlight = as_int(v);
        else if (n == "name")             r.name = as_str(v);
    }
    vec->push_back(std::move(r));
    return 0;
}

int wb_callback(void* out_ptr, int cols, char** vals, char** names) {
    auto* vec = static_cast<std::vector<WhiteBalanceRow>*>(out_ptr);
    WhiteBalanceRow r{};
    auto ai = [&](const char* v) { return v ? std::atoi(v) : 0; };
    for (int i = 0; i < cols; ++i) {
        std::string n = names[i] ? names[i] : "";
        const char* v = vals[i];
        if      (n == "tvin")    r.tvin = ai(v);
        else if (n == "mode")    r.mode = ai(v);
        else if (n == "RGain")   r.rgain = ai(v);
        else if (n == "GGain")   r.ggain = ai(v);
        else if (n == "BGain")   r.bgain = ai(v);
        else if (n == "ROffset") r.roffset = ai(v);
        else if (n == "GOffset") r.goffset = ai(v);
        else if (n == "BOffset") r.boffset = ai(v);
    }
    vec->push_back(std::move(r));
    return 0;
}

int gamma_callback(void* out_ptr, int cols, char** vals, char**) {
    auto* gp = static_cast<GammaPoints*>(out_ptr);
    if (cols < 2) return 0;
    int id  = vals[0] ? std::atoi(vals[0]) : 0;
    int val = vals[1] ? std::atoi(vals[1]) : 0;
    if (id >= 0 && id < static_cast<int>(gp->size()))
        (*gp)[id] = static_cast<int16_t>(val);
    return 0;
}

} // namespace

int load_tvpq_db(const std::string& path, Config& out) {
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        std::fprintf(stderr, "pqconfig: sqlite3_open(%s): %s\n",
                     path.c_str(), db ? sqlite3_errmsg(db) : "null");
        if (db) sqlite3_close(db);
        return -ENOENT;
    }

    auto run = [&](const char* sql, int(*cb)(void*,int,char**,char**), void* ctx) {
        char* err = nullptr;
        int rc = sqlite3_exec(db, sql, cb, ctx, &err);
        if (rc != SQLITE_OK) {
            std::fprintf(stderr, "pqconfig: sqlite3_exec(%s): %s\n",
                         sql, err ? err : "?");
            sqlite3_free(err);
        }
    };

    run("SELECT * FROM Picture_Mode;",      pm_callback,    &out.picture_modes);
    run("SELECT * FROM White_Balance_Mode;", wb_callback,   &out.white_balance);
    run("SELECT id, value FROM Gamma_Point ORDER BY id;",
        gamma_callback, &out.gamma_points);

    sqlite3_close(db);
    return 0;
}

/* --------------------------------------------------------------- *
 * XML — pqcontrol_custom_setting.xml
 *
 * Schema is very flat; we use regex instead of pulling in a full XML
 * library.  The file is ~1 KB and single-level.
 * --------------------------------------------------------------- */

namespace {

/* Parse a `key="value"` pair into an int; returns default if not found. */
int attr_int(const std::string& xml, const std::string& tag_key, int def) {
    std::regex re(tag_key + R"(\s*=\s*\"(-?\d+)\")");
    std::smatch m;
    if (std::regex_search(xml, m, re))
        return std::atoi(m[1].str().c_str());
    return def;
}

/* Parse string attribute. */
std::string attr_str(const std::string& xml, const std::string& tag_key,
                     const std::string& def)
{
    std::regex re(tag_key + R"(\s*=\s*\"([^\"]*)\")");
    std::smatch m;
    if (std::regex_search(xml, m, re))
        return m[1].str();
    return def;
}

/* Extract a single named <tag ... /> block and parse its attrs into
 * a CustomSetting::Sliders. */
void parse_sliders(const std::string& xml, const std::string& tag,
                   CustomSetting::Sliders& out)
{
    std::regex block_re("<" + tag + R"(([^/>]*))");
    std::smatch m;
    if (!std::regex_search(xml, m, block_re)) return;
    std::string blk = m[1].str();

    out.cvbs_pedestal_mode = attr_int(blk, "cvbs_pedestal_mode", out.cvbs_pedestal_mode);
    out.brightness         = attr_int(blk, "brightness",         out.brightness);
    out.contrast           = attr_int(blk, "contrast",           out.contrast);
    out.saturation         = attr_int(blk, "saturation",         out.saturation);
    out.hue                = attr_int(blk, "hue",                out.hue);
    out.sharpness          = attr_int(blk, "sharpness",          out.sharpness);
    out.backlight          = attr_int(blk, "backlight",          out.backlight);
    out.colortemperature   = attr_int(blk, "colortemperature",   out.colortemperature);
    out.gamma              = attr_int(blk, "gamma",              out.gamma);
    out.tnr                = attr_int(blk, "tnr",                out.tnr);
    out.snr                = attr_int(blk, "snr",                out.snr);
    out.dci                = attr_int(blk, "dci",                out.dci);
    out.blackextension     = attr_int(blk, "blackextenstion",    out.blackextension); /* stock typo */
    out.dynamic_backlight  = attr_int(blk, "dynamic_backlight",  out.dynamic_backlight);
    out.device_mode        = attr_int(blk, "device_mode",        out.device_mode);
}

} // namespace

int load_custom_setting_xml(const std::string& path, Config& out) {
    std::ifstream f(path);
    if (!f.is_open()) return -ENOENT;
    std::string xml((std::istreambuf_iterator<char>(f)),
                     std::istreambuf_iterator<char>());

    /* Top-level single attrs */
    std::regex src_re(R"(<current_source_type[^/>]*tvin\s*=\s*\"(\d+)\")");
    std::smatch m;
    if (std::regex_search(xml, m, src_re))
        out.custom.current_source_type = std::atoi(m[1].str().c_str());

    parse_sliders(xml, "current_data",    out.custom.current_data);
    parse_sliders(xml, "custom_videodec", out.custom.custom_videodec);
    parse_sliders(xml, "custom_hdmi1",    out.custom.custom_hdmi1);
    parse_sliders(xml, "custom_hdmi2",    out.custom.custom_hdmi2);
    parse_sliders(xml, "custom_cvbs",     out.custom.custom_cvbs);

    /* Mode names */
    std::regex cm_re(R"(<current_mode([^/>]*)/>)");
    if (std::regex_search(xml, m, cm_re)) {
        std::string blk = m[1].str();
        out.custom.mode_videodec = attr_str(blk, "mode_videodec", "standard");
        out.custom.mode_hdmi1    = attr_str(blk, "mode_hdmi1",    "standard");
        out.custom.mode_hdmi2    = attr_str(blk, "mode_hdmi2",    "standard");
        out.custom.mode_cvbs     = attr_str(blk, "mode_cvbs",     "standard");
    }

    return 0;
}

/* --------------------------------------------------------------- *
 * pq_colortemp.ini — legacy Windows-INI
 * --------------------------------------------------------------- */

int load_colortemp_ini(const std::string& path, Config& out) {
    std::ifstream f(path);
    if (!f.is_open()) return -ENOENT;

    auto map_section = [](const std::string& s) -> std::optional<SourceType> {
        if (s == "COLOR_TEMP_VIDEODEC") return SourceType::VIDEODEC;
        if (s == "COLOR_TEMP_HDMI")     return SourceType::HDMI1;
        if (s == "COLOR_TEMP_CVBS")     return SourceType::CVBS;
        if (s == "COLOR_TEMP_ATV")      return SourceType::ATV;
        if (s == "COLOR_TEMP_DTV")      return SourceType::DTV;
        return std::nullopt;
    };

    std::string line;
    std::optional<SourceType> cur_src;

    while (std::getline(f, line)) {
        /* Strip trailing \r and comments */
        auto hash = line.find_first_of("#;");
        if (hash != std::string::npos) line.erase(hash);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.pop_back();
        /* Strip leading WS */
        size_t lead = line.find_first_not_of(" \t");
        if (lead == std::string::npos) continue;
        line.erase(0, lead);
        if (line.empty()) continue;

        if (line.front() == '[' && line.back() == ']') {
            cur_src = map_section(line.substr(1, line.size() - 2));
            continue;
        }
        if (!cur_src) continue;

        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string rhs = line.substr(eq + 1);

        /* Expect: RO,GO,BO,RG,GG,BG */
        int vals[6] = {0,0,0,0,0,0};
        size_t pos = 0;
        for (int i = 0; i < 6 && pos < rhs.size(); ++i) {
            size_t comma = rhs.find(',', pos);
            vals[i] = std::atoi(rhs.substr(pos, comma == std::string::npos
                                               ? rhs.size() - pos : comma - pos).c_str());
            if (comma == std::string::npos) break;
            pos = comma + 1;
        }
        ColorTempEntry e{};
        e.mode_name = key;
        e.roffset = vals[0]; e.goffset = vals[1]; e.boffset = vals[2];
        e.rgain   = vals[3]; e.ggain   = vals[4]; e.bgain   = vals[5];
        out.color_temp[*cur_src].push_back(std::move(e));
    }
    return 0;
}

/* --------------------------------------------------------------- *
 * load_all
 * --------------------------------------------------------------- */

int load_all(const std::string& base_dir, Config& out) {
    int errors = 0;
    if (load_tvpq_db(base_dir + "/tvpq.db", out) < 0) ++errors;
    if (load_custom_setting_xml(base_dir + "/pqcontrol_custom_setting.xml", out) < 0) ++errors;
    if (load_colortemp_ini(base_dir + "/pq_colortemp.ini", out) < 0) ++errors;
    return errors;  /* caller decides what's fatal */
}

} // namespace pqconfig
} // namespace hy310
