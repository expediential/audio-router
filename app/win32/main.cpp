#include "syncaudio/core/device_descriptor.h"
#include "syncaudio/core/endpoint_volume.h"
#include "syncaudio/core/wasapi_mirror.h"

#include <windows.h>
#include <commctrl.h>
#include <objbase.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr wchar_t window_class_name[] = L"SyncAudioDashboard";
constexpr UINT message_route_status = WM_APP + 42;
constexpr int id_device_list = 100;
constexpr int id_refresh = 101;
constexpr int id_start = 102;
constexpr int id_stop = 103;
constexpr int id_volume = 104;
constexpr int id_mute = 105;

class RouteWorker {
public:
    RouteWorker(HWND dashboard, std::wstring endpoint_id, std::wstring name)
        : dashboard_(dashboard), endpoint_id_(std::move(endpoint_id)), name_(std::move(name)) {}

    void start() {
        worker_ = std::jthread([this](std::stop_token stop_token) {
            const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            if (FAILED(initialized)) {
                post(L"Could not initialize audio routing for " + name_ + L".");
                return;
            }
            try {
                syncaudio::WasapiMirror mirror(endpoint_id_);
                mirror.prepare();
                mirror.start();
                post(L"Routing system audio to " + name_ + L".");
                while (!stop_token.stop_requested()) mirror.service_once(250);
                mirror.stop();
                post(L"Stopped routing to " + name_ + L".");
            } catch (const std::exception& error) {
                std::wstring reason(error.what(), error.what() + std::strlen(error.what()));
                post(name_ + L": " + reason);
            }
            CoUninitialize();
        });
    }

    void stop() {
        if (worker_.joinable()) {
            worker_.request_stop();
            worker_.join();
        }
    }

    ~RouteWorker() { stop(); }

private:
    void post(std::wstring message) const {
        if (IsWindow(dashboard_)) PostMessageW(dashboard_, message_route_status, 0,
                                                reinterpret_cast<LPARAM>(new std::wstring(std::move(message))));
    }

    HWND dashboard_;
    std::wstring endpoint_id_;
    std::wstring name_;
    std::jthread worker_;
};

class Dashboard {
public:
    explicit Dashboard(HWND window) : window_(window) {
        title_font_ = CreateFontW(-30, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                  OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                  DEFAULT_PITCH, L"Segoe UI");
        body_font_ = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                 OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                 DEFAULT_PITCH, L"Segoe UI");
        small_font_ = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                  OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                  DEFAULT_PITCH, L"Segoe UI");
        create_controls();
        refresh_devices();
    }

    ~Dashboard() {
        stop_routes();
        if (title_font_ != nullptr) DeleteObject(title_font_);
        if (body_font_ != nullptr) DeleteObject(body_font_);
        if (small_font_ != nullptr) DeleteObject(small_font_);
        if (background_brush_ != nullptr) DeleteObject(background_brush_);
        if (status_brush_ != nullptr) DeleteObject(status_brush_);
    }

    void resize(int width, int height) {
        const int margin = 28;
        const int top = 112;
        const int panel_width = (std::max)(330, width * 36 / 100);
        const int list_width = width - panel_width - margin * 3;
        const int content_height = (std::max)(320, height - top - 86);
        MoveWindow(title_, margin, 22, width - margin * 2, 40, TRUE);
        MoveWindow(subtitle_, margin, 59, width - margin * 2, 26, TRUE);
        MoveWindow(list_label_, margin, top - 31, list_width, 24, TRUE);
        MoveWindow(refresh_, margin + list_width - 104, top - 35, 104, 28, TRUE);
        MoveWindow(list_, margin, top, list_width, content_height, TRUE);
        const int panel_x = margin * 2 + list_width;
        MoveWindow(panel_title_, panel_x, top - 31, panel_width, 24, TRUE);
        MoveWindow(selected_name_, panel_x, top + 12, panel_width, 48, TRUE);
        MoveWindow(selected_detail_, panel_x, top + 65, panel_width, 42, TRUE);
        MoveWindow(volume_label_, panel_x, top + 128, panel_width, 24, TRUE);
        MoveWindow(volume_, panel_x, top + 156, panel_width, 34, TRUE);
        MoveWindow(mute_, panel_x, top + 202, 110, 34, TRUE);
        MoveWindow(start_, panel_x, top + 268, panel_width, 42, TRUE);
        MoveWindow(stop_, panel_x, top + 320, panel_width, 34, TRUE);
        MoveWindow(route_note_, panel_x, top + 374, panel_width, 88, TRUE);
        MoveWindow(status_, margin, height - 45, width - margin * 2, 26, TRUE);
    }

    void refresh_devices() {
        const auto keep = selected_ids();
        devices_ = syncaudio::enumerate_render_endpoints();
        ListView_DeleteAllItems(list_);
        for (int index = 0; index < static_cast<int>(devices_.size()); ++index) {
            const auto& device = devices_[index];
            LVITEMW item{};
            item.mask = LVIF_TEXT | LVIF_PARAM;
            item.iItem = index;
            item.pszText = const_cast<LPWSTR>(device.name.empty() ? L"Unnamed endpoint" : device.name.c_str());
            item.lParam = index;
            ListView_InsertItem(list_, &item);
            set_column(index, 1, syncaudio::to_string(device.state));
            const std::wstring format = device.sample_rate == 0
                ? L"Unavailable" : std::to_wstring(device.sample_rate) + L" Hz / " + std::to_wstring(device.channels) + L" ch";
            set_column(index, 2, format.c_str());
            if (std::find(keep.begin(), keep.end(), device.id) != keep.end() &&
                device.state == syncaudio::EndpointState::active && !device.is_default_multimedia) {
                ListView_SetCheckState(list_, index, TRUE);
            }
        }
        update_selection_controls();
        set_status(L"Ready. Select up to five active destination devices.");
    }

    void on_command(WORD control_id, WORD notification) {
        if (control_id == id_refresh && notification == BN_CLICKED) refresh_devices();
        if (control_id == id_start && notification == BN_CLICKED) start_routes();
        if (control_id == id_stop && notification == BN_CLICKED) {
            stop_routes();
            set_status(L"Audio sharing stopped. Windows audio routing has been released.");
        }
        if (control_id == id_mute && notification == BN_CLICKED) toggle_mute();
        if (control_id == id_volume && notification == TB_THUMBPOSITION) set_volume_from_slider();
    }

    void on_scroll(HWND source, UINT code) {
        if (source == volume_ && (code == TB_THUMBTRACK || code == TB_ENDTRACK || code == TB_LINEUP ||
                                  code == TB_LINEDOWN || code == TB_PAGEUP || code == TB_PAGEDOWN)) {
            set_volume_from_slider();
        }
    }

    void on_notify(NMHDR* notification) {
        if (notification->idFrom != id_device_list) return;
        if (notification->code == LVN_ITEMCHANGED) {
            auto* change = reinterpret_cast<NMLISTVIEW*>(notification);
            if (change->uChanged & LVIF_STATE) {
                enforce_selection(change->iItem);
                update_selection_controls();
            }
        }
    }

    void route_status(std::wstring* message) {
        std::unique_ptr<std::wstring> ownership(message);
        if (ownership) set_status(*ownership);
    }

    HBRUSH control_brush(HDC context, bool is_status) {
        SetBkColor(context, is_status ? RGB(28, 37, 54) : RGB(20, 24, 31));
        SetTextColor(context, is_status ? RGB(180, 213, 255) : RGB(232, 235, 241));
        return is_status ? status_brush_ : background_brush_;
    }

    void draw_button(const DRAWITEMSTRUCT& drawing) const {
        const bool pressed = (drawing.itemState & ODS_SELECTED) != 0;
        const bool disabled = (drawing.itemState & ODS_DISABLED) != 0;
        const COLORREF fill = disabled ? RGB(54, 60, 70) :
                              (drawing.CtlID == id_start ? (pressed ? RGB(37, 100, 196) : RGB(54, 126, 234))
                                                          : (pressed ? RGB(49, 56, 69) : RGB(39, 45, 56)));
        HBRUSH brush = CreateSolidBrush(fill);
        FillRect(drawing.hDC, &drawing.rcItem, brush);
        DeleteObject(brush);
        SetBkMode(drawing.hDC, TRANSPARENT);
        SetTextColor(drawing.hDC, disabled ? RGB(140, 145, 154) : RGB(245, 247, 251));
        SelectObject(drawing.hDC, body_font_);
        wchar_t text[64]{};
        GetWindowTextW(drawing.hwndItem, text, static_cast<int>(std::size(text)));
        DrawTextW(drawing.hDC, text, -1, const_cast<RECT*>(&drawing.rcItem), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

private:
    void create_controls() {
        background_brush_ = CreateSolidBrush(RGB(20, 24, 31));
        status_brush_ = CreateSolidBrush(RGB(28, 37, 54));
        title_ = label(L"SyncAudio", body_font_);
        SendMessageW(title_, WM_SETFONT, reinterpret_cast<WPARAM>(title_font_), TRUE);
        subtitle_ = label(L"Choose your audio group, control real endpoint volume, and start sharing.", small_font_);
        list_label_ = label(L"AVAILABLE AUDIO DEVICES", small_font_);
        panel_title_ = label(L"DEVICE CONTROL", small_font_);
        selected_name_ = label(L"Select a device", body_font_);
        selected_detail_ = label(L"Only active, non-default endpoints can join the sharing group.", small_font_);
        volume_label_ = label(L"Endpoint volume", small_font_);
        route_note_ = label(L"Routing uses real WASAPI loopback streams. The current engine starts one isolated pipeline per selected device; hardware clock synchronization is still being developed.", small_font_);
        status_ = label(L"Starting...", small_font_);

        refresh_ = button(L"Refresh", id_refresh);
        start_ = button(L"START AUDIO SHARING", id_start);
        stop_ = button(L"Stop sharing", id_stop);
        mute_ = button(L"Mute", id_mute);
        EnableWindow(stop_, FALSE);
        EnableWindow(mute_, FALSE);

        list_ = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | LVS_REPORT |
                                LVS_SHOWSELALWAYS | LVS_SINGLESEL, 0, 0, 0, 0, window_,
                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(id_device_list)), GetModuleHandleW(nullptr), nullptr);
        ListView_SetExtendedListViewStyle(list_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_CHECKBOXES);
        ListView_SetBkColor(list_, RGB(27, 32, 41));
        ListView_SetTextBkColor(list_, RGB(27, 32, 41));
        ListView_SetTextColor(list_, RGB(231, 235, 242));
        insert_column(0, L"Device", 250);
        insert_column(1, L"Status", 105);
        insert_column(2, L"Format", 130);

        volume_ = CreateWindowExW(0, TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS,
                                  0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id_volume)), GetModuleHandleW(nullptr), nullptr);
        SendMessageW(volume_, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
        SendMessageW(volume_, TBM_SETPAGESIZE, 0, 10);
        EnableWindow(volume_, FALSE);
    }

    HWND label(const wchar_t* text, HFONT font) {
        HWND control = CreateWindowExW(0, WC_STATICW, text, WS_CHILD | WS_VISIBLE, 0, 0, 0, 0,
                                       window_, nullptr, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        return control;
    }

    HWND button(const wchar_t* text, int id) {
        HWND control = CreateWindowExW(0, WC_BUTTONW, text, WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                                       0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                       GetModuleHandleW(nullptr), nullptr);
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(body_font_), TRUE);
        return control;
    }

    void insert_column(int index, const wchar_t* text, int width) {
        LVCOLUMNW column{};
        column.mask = LVCF_TEXT | LVCF_WIDTH;
        column.pszText = const_cast<LPWSTR>(text);
        column.cx = width;
        ListView_InsertColumn(list_, index, &column);
    }

    void set_column(int row, int column, const wchar_t* text) {
        LVITEMW item{};
        item.iSubItem = column;
        item.pszText = const_cast<LPWSTR>(text);
        ListView_SetItemText(list_, row, column, item.pszText);
    }

    [[nodiscard]] std::vector<int> checked_indices() const {
        std::vector<int> result;
        for (int index = 0; index < static_cast<int>(devices_.size()); ++index) {
            if (ListView_GetCheckState(list_, index)) result.push_back(index);
        }
        return result;
    }

    [[nodiscard]] std::vector<std::wstring> selected_ids() const {
        std::vector<std::wstring> ids;
        for (const int index : checked_indices()) ids.push_back(devices_[index].id);
        return ids;
    }

    [[nodiscard]] int focused_index() const { return ListView_GetNextItem(list_, -1, LVNI_SELECTED); }

    void enforce_selection(int index) {
        if (index < 0 || index >= static_cast<int>(devices_.size()) || !ListView_GetCheckState(list_, index)) return;
        const auto& device = devices_[index];
        const bool invalid = device.state != syncaudio::EndpointState::active || device.is_default_multimedia;
        if (invalid || checked_indices().size() > 5) {
            ListView_SetCheckState(list_, index, FALSE);
            set_status(invalid ? L"Choose an active non-default endpoint. The default device is already the loopback source."
                               : L"A sharing group can contain at most five output devices.");
        }
    }

    void update_selection_controls() {
        const int index = focused_index();
        if (index < 0 || index >= static_cast<int>(devices_.size())) {
            SetWindowTextW(selected_name_, L"Select a device");
            SetWindowTextW(selected_detail_, L"Select a row to read or adjust that endpoint's real Windows volume.");
            EnableWindow(volume_, FALSE);
            EnableWindow(mute_, FALSE);
            return;
        }
        const auto& device = devices_[index];
        SetWindowTextW(selected_name_, device.name.empty() ? L"Unnamed endpoint" : device.name.c_str());
        const std::wstring description = std::wstring(syncaudio::to_string(device.state)) + L"  |  " +
            (device.sample_rate ? std::to_wstring(device.sample_rate) + L" Hz / " + std::to_wstring(device.channels) + L" channels"
                                : L"No active shared-mode format");
        SetWindowTextW(selected_detail_, description.c_str());
        try {
            const auto state = syncaudio::endpoint_volume_state(device.id);
            SendMessageW(volume_, TBM_SETPOS, TRUE, static_cast<LPARAM>(state.scalar * 100.0F + 0.5F));
            SetWindowTextW(volume_label_, (L"Endpoint volume: " + std::to_wstring(static_cast<int>(state.scalar * 100.0F + 0.5F)) + L"%").c_str());
            SetWindowTextW(mute_, state.muted ? L"Unmute" : L"Mute");
            EnableWindow(volume_, TRUE);
            EnableWindow(mute_, TRUE);
        } catch (const std::exception&) {
            SetWindowTextW(volume_label_, L"Endpoint volume unavailable");
            EnableWindow(volume_, FALSE);
            EnableWindow(mute_, FALSE);
        }
    }

    void set_volume_from_slider() {
        const int index = focused_index();
        if (index < 0 || index >= static_cast<int>(devices_.size())) return;
        try {
            const auto value = static_cast<float>(SendMessageW(volume_, TBM_GETPOS, 0, 0)) / 100.0F;
            syncaudio::set_endpoint_volume(devices_[index].id, value);
            SetWindowTextW(volume_label_, (L"Endpoint volume: " + std::to_wstring(static_cast<int>(value * 100.0F + 0.5F)) + L"%").c_str());
        } catch (const std::exception& error) {
            set_status(L"Unable to adjust volume: " + narrow_to_wide(error.what()));
        }
    }

    void toggle_mute() {
        const int index = focused_index();
        if (index < 0 || index >= static_cast<int>(devices_.size())) return;
        try {
            const auto state = syncaudio::endpoint_volume_state(devices_[index].id);
            syncaudio::set_endpoint_mute(devices_[index].id, !state.muted);
            SetWindowTextW(mute_, state.muted ? L"Mute" : L"Unmute");
        } catch (const std::exception& error) {
            set_status(L"Unable to change mute: " + narrow_to_wide(error.what()));
        }
    }

    void start_routes() {
        const auto selected = checked_indices();
        if (selected.empty()) {
            set_status(L"Select at least one active destination device before starting audio sharing.");
            return;
        }
        stop_routes();
        for (const int index : selected) {
            const auto& device = devices_[index];
            auto worker = std::make_unique<RouteWorker>(window_, device.id, device.name);
            worker->start();
            routes_.push_back(std::move(worker));
        }
        EnableWindow(start_, FALSE);
        EnableWindow(stop_, TRUE);
        set_status(L"Starting " + std::to_wstring(routes_.size()) + L" isolated audio route(s)...");
    }

    void stop_routes() {
        for (auto& route : routes_) route->stop();
        routes_.clear();
        EnableWindow(start_, TRUE);
        EnableWindow(stop_, FALSE);
    }

    void set_status(const std::wstring& text) { SetWindowTextW(status_, text.c_str()); }

    [[nodiscard]] static std::wstring narrow_to_wide(const char* text) {
        if (text == nullptr) return L"Unknown error.";
        return std::wstring(text, text + std::strlen(text));
    }

    HWND window_{};
    HWND title_{}; HWND subtitle_{}; HWND list_label_{}; HWND panel_title_{}; HWND selected_name_{};
    HWND selected_detail_{}; HWND volume_label_{}; HWND route_note_{}; HWND status_{};
    HWND refresh_{}; HWND start_{}; HWND stop_{}; HWND mute_{}; HWND list_{}; HWND volume_{};
    HFONT title_font_{}; HFONT body_font_{}; HFONT small_font_{};
    HBRUSH background_brush_{}; HBRUSH status_brush_{};
    std::vector<syncaudio::DeviceDescriptor> devices_;
    std::vector<std::unique_ptr<RouteWorker>> routes_;
};

Dashboard* get_dashboard(HWND window) { return reinterpret_cast<Dashboard*>(GetWindowLongPtrW(window, GWLP_USERDATA)); }

LRESULT CALLBACK window_procedure(HWND window, UINT message, WPARAM w_param, LPARAM l_param) {
    Dashboard* dashboard = get_dashboard(window);
    switch (message) {
    case WM_NCCREATE:
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(new Dashboard(window)));
        return TRUE;
    case WM_SIZE:
        if (dashboard) dashboard->resize(LOWORD(l_param), HIWORD(l_param));
        return 0;
    case WM_COMMAND:
        if (dashboard) dashboard->on_command(LOWORD(w_param), HIWORD(w_param));
        return 0;
    case WM_HSCROLL:
        if (dashboard) dashboard->on_scroll(reinterpret_cast<HWND>(l_param), LOWORD(w_param));
        return 0;
    case WM_NOTIFY:
        if (dashboard) dashboard->on_notify(reinterpret_cast<NMHDR*>(l_param));
        return 0;
    case message_route_status:
        if (dashboard) dashboard->route_status(reinterpret_cast<std::wstring*>(l_param));
        else delete reinterpret_cast<std::wstring*>(l_param);
        return 0;
    case WM_DRAWITEM:
        if (dashboard) dashboard->draw_button(*reinterpret_cast<DRAWITEMSTRUCT*>(l_param));
        return TRUE;
    case WM_CTLCOLORSTATIC:
        if (dashboard) return reinterpret_cast<LRESULT>(dashboard->control_brush(reinterpret_cast<HDC>(w_param), reinterpret_cast<HWND>(l_param) == GetDlgItem(window, 0)));
        break;
    case WM_ERASEBKGND:
        if (dashboard) {
            RECT rectangle{}; GetClientRect(window, &rectangle);
            FillRect(reinterpret_cast<HDC>(w_param), &rectangle, dashboard->control_brush(reinterpret_cast<HDC>(w_param), false));
            return 1;
        }
        break;
    case WM_NCDESTROY:
        delete dashboard;
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        return 0;
    }
    return DefWindowProcW(window, message, w_param, l_param);
}

} // namespace

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int command_show) {
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES};
    InitCommonControlsEx(&controls);
    WNDCLASSW window_class{};
    window_class.hInstance = instance;
    window_class.lpszClassName = window_class_name;
    window_class.lpfnWndProc = window_procedure;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hbrBackground = nullptr;
    RegisterClassW(&window_class);
    HWND window = CreateWindowExW(0, window_class_name, L"SyncAudio", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                  CW_USEDEFAULT, CW_USEDEFAULT, 1120, 720, nullptr, nullptr, instance, nullptr);
    ShowWindow(window, command_show);
    UpdateWindow(window);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}
