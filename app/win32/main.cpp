#include "syncaudio/core/device_descriptor.h"
#include "syncaudio/core/endpoint_volume.h"
#include "syncaudio/core/wasapi_mirror.h"

#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <objbase.h>
#include <uxtheme.h>

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
constexpr UINT_PTR device_refresh_timer = 1;

constexpr COLORREF ink = RGB(238, 242, 250);
constexpr COLORREF muted_ink = RGB(165, 177, 198);
constexpr COLORREF canvas = RGB(10, 15, 25);
constexpr COLORREF glass = RGB(25, 34, 51);
constexpr COLORREF glass_highlight = RGB(35, 48, 70);
constexpr COLORREF blue = RGB(98, 151, 255);

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
        const int top = 182;
        const int panel_width = (std::max)(340, width * 35 / 100);
        const int list_width = width - panel_width - margin * 3;
        const int content_height = (std::max)(280, height - top - 84);
        MoveWindow(title_, margin + 26, 33, width - margin * 2 - 52, 40, TRUE);
        MoveWindow(subtitle_, margin + 27, 75, width - margin * 2 - 54, 24, TRUE);
        MoveWindow(group_summary_, width - margin - 236, 41, 205, 30, TRUE);
        MoveWindow(group_hint_, margin + 27, 115, width - margin * 2 - 54, 24, TRUE);
        MoveWindow(list_label_, margin + 22, top - 38, list_width - 44, 24, TRUE);
        MoveWindow(refresh_, margin + list_width - 126, top - 43, 98, 30, TRUE);
        MoveWindow(list_, margin + 18, top, list_width - 36, content_height - 18, TRUE);
        const int panel_x = margin * 2 + list_width;
        MoveWindow(panel_title_, panel_x + 24, top - 38, panel_width - 48, 24, TRUE);
        MoveWindow(selected_name_, panel_x + 24, top + 14, panel_width - 48, 35, TRUE);
        MoveWindow(selected_detail_, panel_x + 24, top + 56, panel_width - 48, 42, TRUE);
        MoveWindow(volume_label_, panel_x + 24, top + 124, panel_width - 48, 24, TRUE);
        MoveWindow(volume_, panel_x + 22, top + 151, panel_width - 44, 34, TRUE);
        MoveWindow(mute_, panel_x + 24, top + 205, 118, 34, TRUE);
        MoveWindow(start_, panel_x + 24, top + 270, panel_width - 48, 46, TRUE);
        MoveWindow(stop_, panel_x + 24, top + 326, panel_width - 48, 34, TRUE);
        MoveWindow(route_note_, panel_x + 24, top + 385, panel_width - 48, 78, TRUE);
        MoveWindow(status_, margin + 24, height - 51, width - margin * 2 - 48, 25, TRUE);
    }

    void refresh_devices(bool announce = true) {
        const auto keep = selected_ids();
        devices_ = syncaudio::enumerate_render_endpoints();
        readiness_.clear();
        readiness_.reserve(devices_.size());
        for (const auto& device : devices_) {
            if (device.is_default_multimedia) {
                readiness_.push_back({false, {}, {}, L"This is the Windows system-audio source."});
            } else if (device.state == syncaudio::EndpointState::active) {
                readiness_.push_back(syncaudio::WasapiMirror::inspect_destination(device.id));
            } else {
                readiness_.push_back({false, {}, {}, L"Connect this device, then refresh."});
            }
        }
        ListView_DeleteAllItems(list_);
        for (int index = 0; index < static_cast<int>(devices_.size()); ++index) {
            const auto& device = devices_[index];
            LVITEMW item{};
            item.mask = LVIF_TEXT | LVIF_PARAM;
            item.iItem = index;
            item.pszText = const_cast<LPWSTR>(device.name.empty() ? L"Unnamed endpoint" : device.name.c_str());
            item.lParam = index;
            ListView_InsertItem(list_, &item);
            const wchar_t* state = device.is_default_multimedia ? L"System source" :
                (readiness_[index].can_start ? L"Ready" : syncaudio::to_string(device.state));
            set_column(index, 1, state);
            const std::wstring format = device.sample_rate == 0
                ? L"Unavailable" : std::to_wstring(device.sample_rate) + L" Hz / " + std::to_wstring(device.channels) + L" ch";
            set_column(index, 2, format.c_str());
            if (std::find(keep.begin(), keep.end(), device.id) != keep.end() &&
                readiness_[index].can_start) {
                ListView_SetCheckState(list_, index, TRUE);
            }
        }
        update_selection_controls();
        update_group_summary();
        if (announce) set_status(L"Ready. Select up to five output devices marked Ready.");
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
                update_group_summary();
            }
        }
        if (notification->code == NM_DBLCLK) {
            const auto* activation = reinterpret_cast<NMITEMACTIVATE*>(notification);
            if (activation->iItem >= 0 && activation->iItem < static_cast<int>(devices_.size())) {
                ListView_SetCheckState(list_, activation->iItem, !ListView_GetCheckState(list_, activation->iItem));
                enforce_selection(activation->iItem);
                update_group_summary();
            }
        }
    }

    void route_status(std::wstring* message) {
        std::unique_ptr<std::wstring> ownership(message);
        if (ownership) set_status(*ownership);
    }

    HBRUSH control_brush(HDC context) {
        SetBkMode(context, TRANSPARENT);
        SetTextColor(context, ink);
        return reinterpret_cast<HBRUSH>(GetStockObject(NULL_BRUSH));
    }

    void draw_button(const DRAWITEMSTRUCT& drawing) const {
        const bool pressed = (drawing.itemState & ODS_SELECTED) != 0;
        const bool disabled = (drawing.itemState & ODS_DISABLED) != 0;
        const COLORREF fill = disabled ? RGB(55, 63, 78) :
                              (drawing.CtlID == id_start ? (pressed ? RGB(58, 111, 211) : blue)
                                                          : (pressed ? RGB(50, 61, 80) : RGB(38, 48, 66)));
        HBRUSH brush = CreateSolidBrush(fill);
        HPEN border = CreatePen(PS_SOLID, 1, disabled ? RGB(63, 71, 86) : RGB(99, 118, 148));
        HGDIOBJ old_pen = SelectObject(drawing.hDC, border);
        HGDIOBJ old_brush = SelectObject(drawing.hDC, brush);
        RoundRect(drawing.hDC, drawing.rcItem.left, drawing.rcItem.top, drawing.rcItem.right, drawing.rcItem.bottom, 16, 16);
        SelectObject(drawing.hDC, old_brush);
        SelectObject(drawing.hDC, old_pen);
        DeleteObject(brush);
        DeleteObject(border);
        SetBkMode(drawing.hDC, TRANSPARENT);
        SetTextColor(drawing.hDC, disabled ? RGB(140, 145, 154) : RGB(245, 247, 251));
        SelectObject(drawing.hDC, body_font_);
        wchar_t text[64]{};
        GetWindowTextW(drawing.hwndItem, text, static_cast<int>(std::size(text)));
        RECT text_rect = drawing.rcItem;
        if (drawing.CtlID == id_start) {
            const int center_y = (drawing.rcItem.top + drawing.rcItem.bottom) / 2;
            POINT play[] = {{drawing.rcItem.left + 21, center_y - 7}, {drawing.rcItem.left + 21, center_y + 7}, {drawing.rcItem.left + 33, center_y}};
            HBRUSH icon = CreateSolidBrush(RGB(255, 255, 255));
            HPEN none = CreatePen(PS_NULL, 0, RGB(0, 0, 0));
            SelectObject(drawing.hDC, icon); SelectObject(drawing.hDC, none); Polygon(drawing.hDC, play, 3);
            DeleteObject(icon); DeleteObject(none);
            text_rect.left += 14;
        }
        DrawTextW(drawing.hDC, text, -1, &text_rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    void paint(HDC context) const {
        RECT client{}; GetClientRect(window_, &client);
        HBRUSH base = CreateSolidBrush(canvas); FillRect(context, &client, base); DeleteObject(base);
        const int width = client.right;
        const int height = client.bottom;
        const int margin = 28;
        const int top = 182;
        const int panel_width = (std::max)(340, width * 35 / 100);
        const int list_width = width - panel_width - margin * 3;
        draw_glass_panel(context, {margin, 20, width - margin, 150}, 25);
        draw_glass_panel(context, {margin, top - 58, margin + list_width, height - 66}, 22);
        draw_glass_panel(context, {margin * 2 + list_width, top - 58, width - margin, height - 66}, 22);
        draw_glass_panel(context, {margin, height - 58, width - margin, height - 18}, 17);
        HPEN separator = CreatePen(PS_SOLID, 1, RGB(58, 74, 101));
        HGDIOBJ previous = SelectObject(context, separator);
        MoveToEx(context, margin + 28, 105, nullptr); LineTo(context, width - margin - 28, 105);
        SelectObject(context, previous); DeleteObject(separator);
        // Small drawn audio mark: avoids external assets while giving the app a
        // distinct hardware-control identity.
        HPEN icon_pen = CreatePen(PS_SOLID, 3, blue); previous = SelectObject(context, icon_pen);
        Arc(context, margin + 25, 36, margin + 51, 62, 0, 49, 0, 49);
        Arc(context, margin + 30, 41, margin + 46, 57, 0, 49, 0, 49);
        SelectObject(context, previous); DeleteObject(icon_pen);
    }

private:
    void create_controls() {
        background_brush_ = CreateSolidBrush(canvas);
        status_brush_ = CreateSolidBrush(glass);
        title_ = label(L"SyncAudio", body_font_);
        SendMessageW(title_, WM_SETFONT, reinterpret_cast<WPARAM>(title_font_), TRUE);
        subtitle_ = label(L"Share system audio with your people - without touching Windows device settings by hand.", small_font_);
        group_summary_ = label(L"0 / 5 selected", small_font_);
        group_hint_ = label(L"1. Pick outputs     2. Tune volume     3. Start sharing", small_font_);
        list_label_ = label(L"PICK OUTPUT DEVICES", small_font_);
        panel_title_ = label(L"TUNE SELECTED DEVICE", small_font_);
        selected_name_ = label(L"Select a device", body_font_);
        selected_detail_ = label(L"Pick a row to see its details and adjust its device volume.", small_font_);
        volume_label_ = label(L"Device volume", small_font_);
        route_note_ = label(L"Sync preview\nYour selected devices play real routed audio. Advanced clock alignment is still being tuned per hardware.", small_font_);
        status_ = label(L"Starting...", small_font_);

        refresh_ = button(L"Refresh", id_refresh);
        start_ = button(L"Start audio sharing", id_start);
        stop_ = button(L"Stop audio sharing", id_stop);
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
        SetWindowTheme(list_, L"DarkMode_Explorer", nullptr);
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
        const bool invalid = device.state != syncaudio::EndpointState::active || device.is_default_multimedia ||
                             !readiness_[index].can_start;
        if (invalid || checked_indices().size() > 5) {
            ListView_SetCheckState(list_, index, FALSE);
            set_status(invalid ? readiness_[index].reason
                               : L"A sharing group can contain at most five output devices.");
        }
    }

    void update_selection_controls() {
        const int index = focused_index();
        if (index < 0 || index >= static_cast<int>(devices_.size())) {
            SetWindowTextW(selected_name_, L"Select a device");
            SetWindowTextW(selected_detail_, L"Click a device row to adjust its real Windows endpoint volume.");
            EnableWindow(volume_, FALSE);
            EnableWindow(mute_, FALSE);
            return;
        }
        const auto& device = devices_[index];
        SetWindowTextW(selected_name_, device.name.empty() ? L"Unnamed endpoint" : device.name.c_str());
        const std::wstring description = readiness_[index].can_start ? L"Ready to share  |  " : readiness_[index].reason + L"  |  " +
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
            const auto now_ready = syncaudio::WasapiMirror::inspect_destination(device.id);
            if (!now_ready.can_start) {
                set_status(device.name + L" is no longer ready: " + now_ready.reason);
                continue;
            }
            auto worker = std::make_unique<RouteWorker>(window_, device.id, device.name);
            worker->start();
            routes_.push_back(std::move(worker));
        }
        if (routes_.empty()) {
            set_status(L"No selected device could be started. Refresh the device list and reconnect it if needed.");
            return;
        }
        EnableWindow(start_, FALSE);
        EnableWindow(stop_, TRUE);
        set_status(L"Starting " + std::to_wstring(routes_.size()) + L" real audio route(s)...");
    }

    void update_group_summary() {
        const auto count = checked_indices().size();
        const std::wstring text = std::to_wstring(count) + L" / 5 selected";
        SetWindowTextW(group_summary_, text.c_str());
    }

    void stop_routes() {
        for (auto& route : routes_) route->stop();
        routes_.clear();
        EnableWindow(start_, TRUE);
        EnableWindow(stop_, FALSE);
    }

    void set_status(const std::wstring& text) { SetWindowTextW(status_, text.c_str()); }

    static void draw_glass_panel(HDC context, RECT rectangle, int radius) {
        HBRUSH body = CreateSolidBrush(glass);
        HPEN outline = CreatePen(PS_SOLID, 1, glass_highlight);
        HGDIOBJ old_body = SelectObject(context, body);
        HGDIOBJ old_outline = SelectObject(context, outline);
        RoundRect(context, rectangle.left, rectangle.top, rectangle.right, rectangle.bottom, radius, radius);
        SelectObject(context, old_outline); SelectObject(context, old_body);
        DeleteObject(outline); DeleteObject(body);
        HPEN sheen = CreatePen(PS_SOLID, 1, RGB(70, 89, 121));
        HGDIOBJ old_sheen = SelectObject(context, sheen);
        MoveToEx(context, rectangle.left + radius, rectangle.top + 1, nullptr);
        LineTo(context, rectangle.right - radius, rectangle.top + 1);
        SelectObject(context, old_sheen); DeleteObject(sheen);
    }

    [[nodiscard]] static std::wstring narrow_to_wide(const char* text) {
        if (text == nullptr) return L"Unknown error.";
        return std::wstring(text, text + std::strlen(text));
    }

    HWND window_{};
    HWND title_{}; HWND subtitle_{}; HWND group_summary_{}; HWND group_hint_{}; HWND list_label_{}; HWND panel_title_{}; HWND selected_name_{};
    HWND selected_detail_{}; HWND volume_label_{}; HWND route_note_{}; HWND status_{};
    HWND refresh_{}; HWND start_{}; HWND stop_{}; HWND mute_{}; HWND list_{}; HWND volume_{};
    HFONT title_font_{}; HFONT body_font_{}; HFONT small_font_{};
    HBRUSH background_brush_{}; HBRUSH status_brush_{};
    std::vector<syncaudio::DeviceDescriptor> devices_;
    std::vector<syncaudio::RouteReadiness> readiness_;
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
    case WM_TIMER:
        if (dashboard && w_param == device_refresh_timer) dashboard->refresh_devices(false);
        return 0;
    case WM_GETMINMAXINFO:
        reinterpret_cast<MINMAXINFO*>(l_param)->ptMinTrackSize = {980, 640};
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
        if (dashboard) return reinterpret_cast<LRESULT>(dashboard->control_brush(reinterpret_cast<HDC>(w_param)));
        break;
    case WM_ERASEBKGND:
        if (dashboard) {
            RECT rectangle{}; GetClientRect(window, &rectangle);
            dashboard->paint(reinterpret_cast<HDC>(w_param));
            return 1;
        }
        break;
    case WM_PAINT:
        if (dashboard) {
            PAINTSTRUCT paint{};
            HDC context = BeginPaint(window, &paint);
            dashboard->paint(context);
            EndPaint(window, &paint);
            return 0;
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
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(com)) return 1;
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
    // Windows 11 supplies the low-cost Mica backdrop; the custom client
    // surfaces provide the readable glass treatment above it.
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(window, 20, &dark, sizeof(dark)); // DWMWA_USE_IMMERSIVE_DARK_MODE
    const int backdrop = 2; // DWMSBT_MAINWINDOW (Mica)
    DwmSetWindowAttribute(window, 38, &backdrop, sizeof(backdrop)); // DWMWA_SYSTEMBACKDROP_TYPE
    SetTimer(window, device_refresh_timer, 4'000, nullptr);
    ShowWindow(window, command_show);
    UpdateWindow(window);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    KillTimer(window, device_refresh_timer);
    CoUninitialize();
    return static_cast<int>(message.wParam);
}
