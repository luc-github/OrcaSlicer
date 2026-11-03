#include "ESP3D.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <thread>
#include <boost/filesystem/path.hpp>
#include <boost/format.hpp>
#include <boost/log/trivial.hpp>
#include <boost/algorithm/string.hpp>
#include <nlohmann/json.hpp>

#include <utility>
#include <vector>
#include <wx/string.h>

#include "PrintHost.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/format.hpp"
#include "Http.hpp"

namespace Slic3r {

namespace {

// ESP3D 3.x 3D printer firmware target ids, as reported by [ESP800] "FWTargetID"
// (see esp3d/src/include/esp3d_defines.h). Any other id (grbl, unknown...) is not supported.
constexpr int ESP3D_FW_MARLIN          = 20;
constexpr int ESP3D_FW_MARLIN_EMBEDDED = 30;
constexpr int ESP3D_FW_SMOOTHIEWARE    = 40;
constexpr int ESP3D_FW_REPETIER        = 50;

// Path prefix used by [ESP700] to stream a file from the ESP SD card
constexpr const char* ESP3D_SD_HEADER = "/SD";

// Printer firmware may not answer right after an upload (and a shared SD card
// needs time to be handed back to the printer), so wait before starting the print
constexpr auto START_PRINT_DELAY = std::chrono::milliseconds(1500);

bool is_printer_target(int id)
{
    return id == ESP3D_FW_MARLIN || id == ESP3D_FW_MARLIN_EMBEDDED || id == ESP3D_FW_SMOOTHIEWARE ||
           id == ESP3D_FW_REPETIER;
}

// Read a JSON value that ESP3D may send either as a string or as a number
std::string json_string(const nlohmann::json& obj, const char* key)
{
    auto it = obj.find(key);
    if (it == obj.end() || it->is_null())
        return {};
    if (it->is_string())
        return it->get<std::string>();
    return it->dump();
}

// Characters accepted in a FAT 8.3 short name (besides letters and digits)
bool is_valid_83_char(char c)
{
    static const std::string extra = "!#$%&'()-@^_`{}~";
    return std::isalnum(static_cast<unsigned char>(c)) || extra.find(c) != std::string::npos;
}

std::string sanitize_83_part(const std::string& in, size_t max_len)
{
    std::string out;
    for (char c : in) {
        if (out.size() >= max_len)
            break;
        out += is_valid_83_char(c) ? static_cast<char>(std::toupper(static_cast<unsigned char>(c))) : '_';
    }
    return out;
}

} // namespace

ESP3D::ESP3D(DynamicPrintConfig* config)
    : m_host(config->opt_string("print_host"))
    , m_user(config->opt_string("printhost_user"))
    , m_password(config->opt_string("printhost_password"))
{}

const char* ESP3D::get_name() const { return "ESP3D"; }

std::string ESP3D::base_url() const
{
    // Historically the host was used without scheme, accept both forms
    std::string host = m_host;
    if (boost::istarts_with(host, "http://"))
        host = host.substr(7);
    else if (boost::istarts_with(host, "https://"))
        host = host.substr(8);
    while (!host.empty() && host.back() == '/')
        host.pop_back();
    return "http://" + host;
}

void ESP3D::apply_auth(Http& http) const
{
    // HTTP basic authentication: accepted by ESP3D 3.x on every endpoint and by
    // ESP3D 2.x (same as http://user:password@host); users are "admin" or "user".
    // Nothing is sent when no password is set
    if (!m_password.empty())
        http.auth_basic(m_user.empty() ? std::string("admin") : m_user, m_password);
}

std::string ESP3D::format_command(const std::string& path, const std::string& arg, const std::string& val) const
{
    return (boost::format("%1%%2%?%3%=%4%") % base_url() % path % arg % val).str();
}

// ---------------------------------------------------------------------------
// Detection
// ---------------------------------------------------------------------------

bool ESP3D::parse_esp800_json(const std::string& body, Capabilities& caps) const
{
    nlohmann::json root = nlohmann::json::parse(body, nullptr, false);
    if (root.is_discarded() || !root.is_object())
        return false;
    if (json_string(root, "cmd") != "800" || !root.contains("data") || !root["data"].is_object())
        return false;

    const nlohmann::json& data = root["data"];
    caps.legacy          = false;
    caps.fw_version      = json_string(data, "FWVersion");
    caps.fw_target       = json_string(data, "FWTarget");
    caps.sd_connection   = json_string(data, "SDConnection");
    caps.serial_protocol = json_string(data, "SerialProtocol");
    caps.authentication  = json_string(data, "Authentication") == "Enabled";

    const std::string id = json_string(data, "FWTargetID");
    try {
        caps.fw_target_id = id.empty() ? -1 : std::stoi(id);
    } catch (...) {
        caps.fw_target_id = -1;
    }
    caps.detected = true;
    return true;
}

bool ESP3D::parse_esp800_legacy(const std::string& body, Capabilities& caps) const
{
    // ESP3D 2.x answer, single line with '#' separators:
    // FW version:0.9.200 # FW target:smoothieware # FW HW:Direct SD # primary sd:/ext/ # ...
    if (body.find("FW version:") == std::string::npos)
        return false;

    std::vector<std::string> fields;
    boost::split(fields, body, boost::is_any_of("#"));
    for (std::string field : fields) {
        boost::trim(field);
        const size_t sep = field.find(':');
        if (sep == std::string::npos)
            continue;
        std::string key   = boost::trim_copy(field.substr(0, sep));
        std::string value = boost::trim_copy(field.substr(sep + 1));
        boost::to_lower(key);
        if (key == "fw version")
            caps.fw_version = value;
        else if (key == "fw target")
            caps.fw_target = value;
        else if (key == "fw hw")
            caps.sd_connection = value;
        else if (key == "authentication")
            caps.authentication = boost::iequals(value, "yes");
    }
    caps.legacy   = true;
    caps.detected = true;
    return true;
}

void ESP3D::resolve_methods(Capabilities& caps) const
{
    if (caps.legacy) {
        // ESP3D 2.x: keep the historical behaviour unchanged
        caps.upload_method = UploadMethod::LegacySerial;
        caps.start_method  = StartMethod::PrinterSD;
        return;
    }

    // Upload target
    if (boost::iequals(caps.serial_protocol, "MKS"))
        caps.upload_method = UploadMethod::MKS;
    else if (caps.sd_connection == "direct" || caps.sd_connection == "shared")
        caps.upload_method = UploadMethod::SD;
    else
        caps.upload_method = UploadMethod::None;

    // Start method (unsupported targets are rejected earlier by check_target())
    if (caps.upload_method == UploadMethod::None || !is_printer_target(caps.fw_target_id)) {
        caps.start_method = StartMethod::None;
    } else if (caps.upload_method == UploadMethod::MKS) {
        // File is stored on the printer side by the MKS protocol
        caps.start_method = StartMethod::PrinterSD;
    } else if (caps.sd_connection == "shared") {
        // Includes ESP3DLib (Marlin embedded), which uses a shared SD card
        caps.start_method = StartMethod::SharedSD;
    } else {
        caps.start_method = StartMethod::ESPStream;
    }
}

bool ESP3D::detect(Capabilities& caps, wxString& msg) const
{
    caps = Capabilities();
    bool        auth_error = false;
    bool        net_error  = false;
    std::string error_text;

    // 1 - ESP3D 3.x: /command?cmd=[ESP800]json=yes
    {
        auto http = Http::get(format_command("/command", "cmd", Http::url_encode("[ESP800]json=yes")));
        apply_auth(http);
        http.on_complete([&](std::string body, unsigned) { parse_esp800_json(body, caps); })
            .on_error([&](std::string body, std::string error, unsigned status) {
                if (status == 401) {
                    // Only ESP3D 3.x rejects /command for a guest
                    auth_error = true;
                } else if (status == 0) {
                    net_error  = true;
                    error_text = GUI::into_u8(format_error(body, error, status));
                }
            })
            .perform_sync();
    }

    if (auth_error) {
        msg = m_password.empty() ? _L("ESP3D authentication is enabled: please set the user and password.") :
                                   _L("ESP3D rejected the user or password.");
        return false;
    }
    if (net_error) {
        msg = GUI::from_u8(error_text);
        return false;
    }

    // 2 - ESP3D 2.x: /command?plain=[ESP800], plain text answer
    if (!caps.detected) {
        auto http = Http::get(format_command("/command", "plain", Http::url_encode("[ESP800]")));
        apply_auth(http);
        http.on_complete([&](std::string body, unsigned) { parse_esp800_legacy(body, caps); })
            .on_error([&](std::string body, std::string error, unsigned status) {
                error_text = GUI::into_u8(format_error(body, error, status));
            })
            .perform_sync();
    }

    if (!caps.detected) {
        msg = error_text.empty() ? _L("The device did not answer as an ESP3D board.") : GUI::from_u8(error_text);
        return false;
    }

    resolve_methods(caps);
    BOOST_LOG_TRIVIAL(info) << boost::format("ESP3D: version %1%, target %2% (%3%), SD %4%, protocol %5%, legacy %6%") %
                                   caps.fw_version % caps.fw_target % caps.fw_target_id % caps.sd_connection %
                                   caps.serial_protocol % caps.legacy;
    return true;
}

bool ESP3D::check_target(const Capabilities& caps, wxString& msg) const
{
    // Only 3D printer firmwares are supported; grbl / unknown targets
    // are rejected before any upload so the user does not wait for nothing
    if (caps.legacy || is_printer_target(caps.fw_target_id))
        return true;
    const std::string name = caps.fw_target.empty() ? std::to_string(caps.fw_target_id) : caps.fw_target;
    msg = GUI::format_wxstr(_L("ESP3D target not supported: %s"), name);
    return false;
}

// ---------------------------------------------------------------------------
// Test
// ---------------------------------------------------------------------------

bool ESP3D::test(wxString& msg) const
{
    Capabilities caps;
    if (!detect(caps, msg) || !check_target(caps, msg))
        return false;

    if (caps.legacy) {
        msg = GUI::format_wxstr(_L("Connected to ESP3D %s (2.x, legacy mode)."), caps.fw_version);
        return true;
    }
    if (caps.upload_method == UploadMethod::None) {
        msg = GUI::format_wxstr(_L("Connected to ESP3D %s, but this board has no storage Orca can upload to."),
                                caps.fw_version);
        return false;
    }
    msg = get_test_ok_msg();
    return true;
}

wxString ESP3D::get_test_ok_msg() const { return _(L("Connection to ESP3D is working correctly.")); }

wxString ESP3D::get_test_failed_msg(wxString& msg) const
{
    return GUI::from_u8((boost::format("%s: %s") % _utf8(L("Could not connect to ESP3D")) % std::string(msg.ToUTF8())).str());
}

// ---------------------------------------------------------------------------
// Upload
// ---------------------------------------------------------------------------

bool ESP3D::upload(PrintHostUpload upload_data, ProgressFn prorgess_fn, ErrorFn error_fn, InfoFn info_fn) const
{
    (void) info_fn;
    Capabilities caps;
    wxString     msg;
    if (!detect(caps, msg) || !check_target(caps, msg)) {
        error_fn(std::move(msg));
        return false;
    }

    const std::string short_name = get_short_name(upload_data.upload_path.string());
    bool              res        = false;

    switch (caps.upload_method) {
    case UploadMethod::LegacySerial: res = upload_legacy(upload_data, short_name, prorgess_fn, error_fn); break;
    case UploadMethod::SD: res = upload_sd(upload_data, short_name, prorgess_fn, error_fn); break;
    case UploadMethod::MKS: res = upload_mks(upload_data, short_name, prorgess_fn, error_fn); break;
    case UploadMethod::None:
    default: error_fn(_L("This ESP3D board has no storage Orca can upload to.")); return false;
    }

    if (!res || upload_data.post_action != PrintHostPostUploadAction::StartPrint)
        return res;

    wxString errormsg;
    if (!start_print(caps, errormsg, short_name)) {
        error_fn(std::move(errormsg));
        return false;
    }
    return true;
}

// Shared progress handler: keep the progress bar alive until on_complete
// (ESP3D can be slow and the start command can only be sent after completion)
static void forward_progress(PrintHost::ProgressFn& progress_fn, Http::Progress progress, bool& cancel, bool& res)
{
    Http::Progress prog = std::move(progress);
    if (prog.ulnow > 0)
        prog.ulnow -= 1;
    progress_fn(std::move(prog), cancel);
    if (cancel) {
        BOOST_LOG_TRIVIAL(info) << "ESP3D: Upload canceled";
        res = false;
    }
}

bool ESP3D::upload_legacy(const PrintHostUpload& upload_data, const std::string& short_name, ProgressFn progress_fn, ErrorFn error_fn) const
{
    // ESP3D 2.x behaviour, unchanged
    bool res  = false;
    auto http = Http::post(base_url() + "/upload_serial");
    apply_auth(http);
    http.header("Connection", "keep-alive")
        .form_add_file("file", upload_data.source_path, short_name)
        .on_complete([&](std::string, unsigned) { res = true; })
        .on_error([&](std::string body, std::string error, unsigned status) {
            BOOST_LOG_TRIVIAL(error) << boost::format("ESP3D: Error uploading file: %1%, HTTP %2%, body: `%3%`") % error % status % body;
            error_fn(format_error(body, error, status));
            res = false;
        })
        .on_progress([&](Http::Progress progress, bool& cancel) { forward_progress(progress_fn, std::move(progress), cancel, res); })
        .perform_sync();
    return res;
}

bool ESP3D::upload_sd(const PrintHostUpload& upload_data, const std::string& short_name, ProgressFn progress_fn, ErrorFn error_fn) const
{
    // ESP3D 3.x SD upload handler. The multipart file name is the target path,
    // "<filename>S" carries the expected size (checked against free space and
    // after write). Form fields must come before the file part.
    const std::string target = "/" + short_name;
    const auto        size   = boost::filesystem::file_size(upload_data.source_path);

    bool res  = false;
    auto http = Http::post(base_url() + "/sdfiles?quiet=yes");
    apply_auth(http);
    http.header("Connection", "keep-alive")
        .form_add("path", "/")
        .form_add(target + "S", std::to_string(size))
        .form_add_file("myfile", upload_data.source_path, target)
        .on_complete([&](std::string body, unsigned) {
            // Upload failures are reported with HTTP 200 and a JSON status
            nlohmann::json root = nlohmann::json::parse(body, nullptr, false);
            const std::string status = (!root.is_discarded() && root.is_object()) ? json_string(root, "status") : std::string();
            if (status == "ok") {
                res = true;
            } else {
                BOOST_LOG_TRIVIAL(error) << boost::format("ESP3D: SD upload failed, body: `%1%`") % body;
                error_fn(GUI::format_wxstr(_L("ESP3D refused the upload: %s"), status.empty() ? body : status));
                res = false;
            }
        })
        .on_error([&](std::string body, std::string error, unsigned status) {
            BOOST_LOG_TRIVIAL(error) << boost::format("ESP3D: Error uploading file: %1%, HTTP %2%, body: `%3%`") % error % status % body;
            error_fn(format_error(body, error, status));
            res = false;
        })
        .on_progress([&](Http::Progress progress, bool& cancel) { forward_progress(progress_fn, std::move(progress), cancel, res); })
        .perform_sync();
    return res;
}

bool ESP3D::upload_mks(const PrintHostUpload& upload_data, const std::string& short_name, ProgressFn progress_fn, ErrorFn error_fn) const
{
    // ESP3D 3.x MKS protocol: file is relayed to the printer storage by frames.
    // Size is passed as "s<filename>"; no leading '/' in the name.
    const auto size = boost::filesystem::file_size(upload_data.source_path);

    bool res  = false;
    auto http = Http::post(base_url() + "/upload");
    apply_auth(http);
    http.header("Connection", "keep-alive")
        .form_add("s" + short_name, std::to_string(size))
        .form_add_file("myfile", upload_data.source_path, short_name)
        .on_complete([&](std::string, unsigned) { res = true; })
        .on_error([&](std::string body, std::string error, unsigned status) {
            BOOST_LOG_TRIVIAL(error) << boost::format("ESP3D: Error uploading file (MKS): %1%, HTTP %2%, body: `%3%`") % error % status % body;
            error_fn(format_error(body, error, status));
            res = false;
        })
        .on_progress([&](Http::Progress progress, bool& cancel) { forward_progress(progress_fn, std::move(progress), cancel, res); })
        .perform_sync();
    return res;
}

// ---------------------------------------------------------------------------
// Start print
// ---------------------------------------------------------------------------

bool ESP3D::send_command(const Capabilities& caps, const std::string& command, wxString& msg, std::string* answer) const
{
    // 2.x uses "plain", 3.x uses "cmd"
    const char* arg  = caps.legacy ? "plain" : "cmd";
    bool        ret  = false;
    auto        http = Http::get(format_command("/command", arg, Http::url_encode(command)));
    apply_auth(http);
    http.on_complete([&](std::string body, unsigned) {
            if (answer)
                *answer = std::move(body);
            ret = true;
        })
        .on_error([&](std::string body, std::string error, unsigned status) {
            msg = format_error(body, error, status);
            ret = false;
        })
        .perform_sync();
    return ret;
}

bool ESP3D::start_print(const Capabilities& caps, wxString& msg, const std::string& filename) const
{
    std::this_thread::sleep_for(START_PRINT_DELAY);

    switch (caps.start_method) {
    case StartMethod::SharedSD:
        // Card was just released by the ESP: ask the printer to remount it
        if (!send_command(caps, "M21", msg))
            return false;
        std::this_thread::sleep_for(START_PRINT_DELAY);
        [[fallthrough]];
    case StartMethod::PrinterSD:
        if (!send_command(caps, "M23 " + filename, msg))
            return false;
        return send_command(caps, "M24", msg);

    case StartMethod::ESPStream: {
        std::string answer;
        if (!send_command(caps, std::string("[ESP700]") + ESP3D_SD_HEADER + "/" + filename + " json=yes", msg, &answer))
            return false;
        nlohmann::json root = nlohmann::json::parse(answer, nullptr, false);
        if (root.is_discarded() || !root.is_object() || json_string(root, "status") != "ok") {
            std::string detail = (!root.is_discarded() && root.is_object()) ? json_string(root, "data") : answer;
            msg = GUI::format_wxstr(_L("ESP3D could not start the print: %s"), detail);
            return false;
        }
        return true;
    }

    case StartMethod::None:
    default: msg = _L("Starting a print is not supported for this ESP3D target."); return false;
    }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Always use an 8.3 name: long names crash some printer firmwares and the
// name must be the same for the upload and for M23 / [ESP700]
std::string ESP3D::get_short_name(const std::string& filename) const
{
    boost::filesystem::path p(filename);
    std::string             stem      = p.stem().string();
    std::string             extension = p.extension().string();
    if (!extension.empty() && extension[0] == '.')
        extension = extension.substr(1);

    std::string short_stem = sanitize_83_part(stem, 8);
    if (short_stem.empty())
        short_stem = "PRINT";
    std::string short_ext = sanitize_83_part(extension, 3);
    return short_ext.empty() ? short_stem : short_stem + "." + short_ext;
}

} // namespace Slic3r
