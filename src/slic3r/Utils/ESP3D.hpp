#ifndef slic3r_ESP3D_hpp_
#define slic3r_ESP3D_hpp_

#include <string>
#include <wx/string.h>

#include "PrintHost.hpp"

namespace Slic3r {
class DynamicPrintConfig;
class Http;

class ESP3D : public PrintHost
{
public:
    explicit ESP3D(DynamicPrintConfig* config);
    ~ESP3D() override = default;

    const char* get_name() const override;

    bool                       test(wxString& curl_msg) const override;
    wxString                   get_test_ok_msg() const override;
    wxString                   get_test_failed_msg(wxString& msg) const override;
    bool                       upload(PrintHostUpload upload_data, ProgressFn prorgess_fn, ErrorFn error_fn, InfoFn info_fn) const override;
    bool                       has_auto_discovery() const override { return false; }
    bool                       can_test() const override { return true; }
    PrintHostPostUploadActions get_post_upload_actions() const override { return PrintHostPostUploadAction::StartPrint; }
    std::string                get_host() const override { return m_host; }

    // How the uploaded file reaches the printer
    enum class UploadMethod {
        None,         // no upload target available on this board
        LegacySerial, // ESP3D 2.x: POST /upload_serial (printer SD through serial)
        SD,           // ESP3D 3.x: POST /sdfiles (SD card wired to the ESP, direct or shared)
        MKS           // ESP3D 3.x with MKS serial protocol: POST /upload
    };

    // How the print is started once the file is uploaded
    enum class StartMethod {
        None,        // no way to start a print
        PrinterSD,   // M23 <file> + M24 sent to the printer (file is on the printer storage)
        SharedSD,    // M21 + M23 <file> + M24 (shared SD: printer must remount the card first)
        ESPStream    // [ESP700]/SD/<file> (direct SD: ESP3D streams the file itself)
    };

    // Board capabilities, as reported by [ESP800]
    struct Capabilities
    {
        bool         detected { false };
        bool         legacy { false };        // true for ESP3D 2.x (plain text [ESP800])
        std::string  fw_version;              // e.g. "3.1.3"
        std::string  fw_target;               // e.g. "marlin", "grbl", "unknown"
        int          fw_target_id { -1 };     // numeric target id (3.x only), -1 when unknown
        std::string  sd_connection;           // "direct", "shared", "none" (3.x) / raw "FW HW" (2.x)
        std::string  serial_protocol;         // "Raw", "MKS", "Socket" (3.x only)
        bool         authentication { false };
        UploadMethod upload_method { UploadMethod::None };
        StartMethod  start_method { StartMethod::None };
    };

private:
    std::string m_host;
    std::string m_user;
    std::string m_password;

    // Query [ESP800] and fill caps; on failure msg holds a user readable error
    bool detect(Capabilities& caps, wxString& msg) const;
    bool parse_esp800_json(const std::string& body, Capabilities& caps) const;
    bool parse_esp800_legacy(const std::string& body, Capabilities& caps) const;
    void resolve_methods(Capabilities& caps) const;
    bool check_target(const Capabilities& caps, wxString& msg) const;

    bool upload_legacy(const PrintHostUpload& upload_data, const std::string& short_name, ProgressFn progress_fn, ErrorFn error_fn) const;
    bool upload_sd(const PrintHostUpload& upload_data, const std::string& short_name, ProgressFn progress_fn, ErrorFn error_fn) const;
    bool upload_mks(const PrintHostUpload& upload_data, const std::string& short_name, ProgressFn progress_fn, ErrorFn error_fn) const;

    bool start_print(const Capabilities& caps, wxString& msg, const std::string& filename) const;
    bool send_command(const Capabilities& caps, const std::string& command, wxString& msg, std::string* answer = nullptr) const;

    void        apply_auth(Http& http) const;
    std::string base_url() const;
    std::string get_short_name(const std::string& filename) const;
    std::string format_command(const std::string& path, const std::string& arg, const std::string& val) const;
};

} // namespace Slic3r

#endif
