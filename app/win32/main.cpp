#include "syncaudio/core/device_descriptor.h"
#include "syncaudio/core/endpoint_volume.h"
#include "syncaudio/core/wasapi_mirror.h"

#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <objbase.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr wchar_t window_class_name[] = L"SyncAudioDashboard";
constexpr UINT message_route_status = WM_APP + 42;
constexpr UINT_PTR device_refresh_timer = 1;
constexpr int maximum_outputs = 5;

// One custom renderer keeps the visual system coherent and avoids native
// control/theme artefacts fighting the dashboard background.
constexpr COLORREF surface = RGB(239, 240, 250);
constexpr COLORREF panel = RGB(245, 246, 253);
constexpr COLORREF panel_selected = RGB(234, 233, 255);
constexpr COLORREF shadow = RGB(207, 211, 230);
constexpr COLORREF highlight = RGB(255, 255, 255);
constexpr COLORREF ink = RGB(42, 47, 69);
constexpr COLORREF muted_ink = RGB(118, 126, 153);
constexpr COLORREF accent = RGB(105, 108, 232);
constexpr COLORREF accent_soft = RGB(224, 224, 255);
constexpr COLORREF good = RGB(59, 158, 112);
constexpr COLORREF warning = RGB(195, 133, 57);
constexpr COLORREF disabled = RGB(158, 164, 184);

struct Rect {
    int left{};
    int top{};
    int right{};
    int bottom{};
    [[nodiscard]] int width() const noexcept { return right - left; }
    [[nodiscard]] int height() const noexcept { return bottom - top; }
    [[nodiscard]] RECT native() const noexcept { return {left, top, right, bottom}; }
    [[nodiscard]] bool contains(POINT point) const noexcept {
        return point.x >= left && point.x < right && point.y >= top && point.y < bottom;
    }
};

struct Layout {
    Rect header;
    Rect device_area;
    Rect control;
    Rect refresh;
    Rect start;
    Rect stop;
    Rect mute;
    Rect slider;
    Rect device_viewport;
};

class RouteWorker {
public:
    RouteWorker(HWND dashboard, std::wstring endpoint_id, std::wstring name)
        : dashboard_(dashboard), endpoint_id_(std::move(endpoint_id)), name_(std::move(name)) {}

    void start() {
        worker_ = std::jthread([this](std::stop_token stop_token) {
            const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            if (FAILED(initialized)) {
                post(L"Could not start the Windows audio engine for " + name_ + L".");
                return;
            }
            try {
                syncaudio::WasapiMirror mirror(endpoint_id_);
                mirror.prepare();
                mirror.start();
                post(name_ + L" is sharing audio.");
                while (!stop_token.stop_requested()) mirror.service_once(250);
                mirror.stop();
                post(name_ + L" stopped sharing.");
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
        if (IsWindow(dashboard_)) {
            PostMessageW(dashboard_, message_route_status, 0,
                         reinterpret_cast<LPARAM>(new std::wstring(std::move(message))));
        }
    }

    HWND dashboard_;
    std::wstring endpoint_id_;
    std::wstring name_;
    std::jthread worker_;
};

class Dashboard {
public:
    explicit Dashboard(HWND window) : window_(window), dpi_(GetDpiForWindow(window)) {
        recreate_fonts();
        refresh_devices(true);
    }

    ~Dashboard() {
        stop_routes();
        delete_font(title_font_);
        delete_font(heading_font_);
        delete_font(body_font_);
        delete_font(small_font_);
    }

    void on_size() { invalidate(); }

    void on_dpi_changed(UINT dpi, const RECT& suggested) {
        dpi_ = dpi;
        recreate_fonts();
        SetWindowPos(window_, nullptr, suggested.left, suggested.top,
                     suggested.right - suggested.left, suggested.bottom - suggested.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        invalidate();
    }

    void on_timer() {
        if (routes_.empty()) refresh_devices(false);
    }

    void on_mouse_wheel(short delta, POINT point) {
        if (!layout().device_viewport.contains(point)) return;
        scroll_y_ = (std::max)(0, scroll_y_ + (delta < 0 ? dp(72) : -dp(72)));
        clamp_scroll();
        invalidate();
    }

    void on_left_down(POINT point) {
        const auto page = layout();
        if (page.refresh.contains(point)) { refresh_devices(true); return; }
        if (page.start.contains(point)) { start_routes(); return; }
        if (page.stop.contains(point)) {
            stop_routes();
            status_ = L"Audio sharing stopped. Your normal Windows audio path is unchanged.";
            invalidate();
            return;
        }
        if (page.mute.contains(point)) { toggle_mute(); return; }
        if (page.slider.contains(point)) {
            dragging_volume_ = true;
            update_volume_from_point(point.x, page.slider);
            SetCapture(window_);
            return;
        }
        const auto card = card_at(point);
        if (card) select_or_toggle_device(*card);
    }

    void on_mouse_move(POINT point) {
        if (dragging_volume_) update_volume_from_point(point.x, layout().slider);
    }

    void on_left_up() {
        if (dragging_volume_) {
            dragging_volume_ = false;
            ReleaseCapture();
        }
    }

    void route_status(std::wstring* message) {
        std::unique_ptr<std::wstring> ownership(message);
        if (!ownership) return;
        status_ = *ownership;
        invalidate();
    }

    void paint(HDC context) {
        RECT client{};
        GetClientRect(window_, &client);
        fill_rect(context, client, surface);
        const auto page = layout();
        draw_panel(context, page.header, dp(25));
        draw_panel(context, page.device_area, dp(25));
        draw_panel(context, page.control, dp(25));
        draw_brand(context, page.header);
        draw_device_area(context, page);
        draw_control_deck(context, page);
    }

private:
    [[nodiscard]] int dp(int logical) const noexcept { return MulDiv(logical, static_cast<int>(dpi_), 96); }

    void recreate_fonts() {
        delete_font(title_font_); delete_font(heading_font_); delete_font(body_font_); delete_font(small_font_);
        title_font_ = create_font(29, FW_SEMIBOLD);
        heading_font_ = create_font(18, FW_SEMIBOLD);
        body_font_ = create_font(14, FW_NORMAL);
        small_font_ = create_font(11, FW_NORMAL);
    }

    [[nodiscard]] HFONT create_font(int size, int weight) const {
        return CreateFontW(-dp(size), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                           OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           DEFAULT_PITCH, L"Segoe UI Variable");
    }

    static void delete_font(HFONT& font) {
        if (font != nullptr) DeleteObject(font);
        font = nullptr;
    }

    [[nodiscard]] Layout layout() const {
        RECT client{};
        GetClientRect(window_, &client);
        const int margin = dp(24), gap = dp(18), header_height = dp(128), control_width = dp(344);
        Layout result;
        result.header = {margin, margin, client.right - margin, margin + header_height};
        result.device_area = {margin, result.header.bottom + gap, client.right - margin - control_width - gap, client.bottom - margin};
        result.control = {result.device_area.right + gap, result.header.bottom + gap, client.right - margin, client.bottom - margin};
        result.refresh = {result.device_area.right - dp(112), result.device_area.top + dp(22), result.device_area.right - dp(22), result.device_area.top + dp(60)};
        result.start = {result.control.left + dp(24), result.control.bottom - dp(160), result.control.right - dp(24), result.control.bottom - dp(108)};
        result.stop = {result.control.left + dp(24), result.control.bottom - dp(94), result.control.right - dp(24), result.control.bottom - dp(52)};
        result.mute = {result.control.left + dp(24), result.control.top + dp(278), result.control.left + dp(156), result.control.top + dp(318)};
        result.slider = {result.control.left + dp(24), result.control.top + dp(232), result.control.right - dp(24), result.control.top + dp(260)};
        result.device_viewport = {result.device_area.left + dp(22), result.device_area.top + dp(86), result.device_area.right - dp(22), result.device_area.bottom - dp(22)};
        return result;
    }

    void refresh_devices(bool announce) {
        const auto remembered_group = selected_ids();
        std::optional<std::wstring> remembered_device;
        if (const auto previous = selected_device_index()) remembered_device = devices_[*previous].id;
        devices_ = syncaudio::enumerate_render_endpoints();
        readiness_.clear(); readiness_.reserve(devices_.size()); selected_.assign(devices_.size(), false);
        for (std::size_t index = 0; index < devices_.size(); ++index) {
            const auto& device = devices_[index];
            if (device.is_default_multimedia) readiness_.push_back({false, {}, {}, L"This is your current Windows system-audio source."});
            else if (device.state == syncaudio::EndpointState::active) readiness_.push_back(syncaudio::WasapiMirror::inspect_destination(device.id));
            else readiness_.push_back({false, {}, {}, L"Connect or turn on this output, then refresh."});
            selected_[index] = readiness_.back().can_start && std::find(remembered_group.begin(), remembered_group.end(), device.id) != remembered_group.end();
        }
        selected_id_.reset();
        if (remembered_device) {
            for (std::size_t index = 0; index < devices_.size(); ++index) if (devices_[index].id == *remembered_device) selected_id_ = static_cast<int>(index);
        }
        if (!selected_id_ && !devices_.empty()) selected_id_ = 0;
        clamp_scroll();
        if (announce) status_ = L"Choose outputs marked Ready. Your current Windows device stays the source.";
        invalidate();
    }

    [[nodiscard]] std::vector<std::wstring> selected_ids() const {
        std::vector<std::wstring> ids;
        for (std::size_t index = 0; index < devices_.size(); ++index) if (index < selected_.size() && selected_[index]) ids.push_back(devices_[index].id);
        return ids;
    }

    [[nodiscard]] int selected_count() const { return static_cast<int>(std::count(selected_.begin(), selected_.end(), true)); }
    [[nodiscard]] int card_height() const noexcept { return dp(116); }
    [[nodiscard]] int card_gap() const noexcept { return dp(14); }
    [[nodiscard]] int cards_per_row(const Layout& page) const noexcept { return page.device_viewport.width() >= dp(720) ? 2 : 1; }

    [[nodiscard]] Rect card_rect(std::size_t index, const Layout& page) const {
        const int columns = cards_per_row(page), gap = card_gap();
        const int width = (page.device_viewport.width() - gap * (columns - 1)) / columns;
        const int row = static_cast<int>(index) / columns, column = static_cast<int>(index) % columns;
        const int x = page.device_viewport.left + column * (width + gap);
        const int y = page.device_viewport.top + row * (card_height() + gap) - scroll_y_;
        return {x, y, x + width, y + card_height()};
    }

    [[nodiscard]] std::optional<int> card_at(POINT point) const {
        const auto page = layout();
        if (!page.device_viewport.contains(point)) return std::nullopt;
        for (std::size_t index = 0; index < devices_.size(); ++index) if (card_rect(index, page).contains(point)) return static_cast<int>(index);
        return std::nullopt;
    }

    void clamp_scroll() {
        const auto page = layout();
        const int columns = cards_per_row(page);
        const int rows = static_cast<int>((devices_.size() + columns - 1) / columns);
        const int content = rows == 0 ? 0 : rows * card_height() + (rows - 1) * card_gap();
        scroll_y_ = std::clamp(scroll_y_, 0, (std::max)(0, content - page.device_viewport.height()));
    }

    void select_or_toggle_device(int index) {
        if (index < 0 || index >= static_cast<int>(devices_.size())) return;
        selected_id_ = index;
        if (!readiness_[index].can_start) {
            status_ = readiness_[index].reason;
            invalidate();
            return;
        }
        if (!selected_[index] && selected_count() >= maximum_outputs) {
            status_ = L"A group can contain up to five outputs. Remove one before adding another.";
            invalidate();
            return;
        }
        selected_[index] = !selected_[index];
        status_ = selected_[index] ? devices_[index].name + L" added to this audio group." : devices_[index].name + L" removed from this audio group.";
        invalidate();
    }

    [[nodiscard]] std::optional<int> selected_device_index() const {
        if (!selected_id_ || *selected_id_ < 0 || *selected_id_ >= static_cast<int>(devices_.size())) return std::nullopt;
        return selected_id_;
    }

    void update_volume_from_point(int x, const Rect& slider) {
        const auto selected = selected_device_index();
        if (!selected) return;
        const float value = std::clamp(static_cast<float>(x - slider.left) / static_cast<float>((std::max)(1, slider.width())), 0.0F, 1.0F);
        try {
            syncaudio::set_endpoint_volume(devices_[*selected].id, value);
            status_ = devices_[*selected].name + L" volume set to " + std::to_wstring(static_cast<int>(value * 100.0F + 0.5F)) + L"%.";
        } catch (const std::exception& error) {
            status_ = L"Windows could not change that volume: " + narrow_to_wide(error.what());
        }
        invalidate();
    }

    void toggle_mute() {
        const auto selected = selected_device_index();
        if (!selected) return;
        try {
            const auto state = syncaudio::endpoint_volume_state(devices_[*selected].id);
            syncaudio::set_endpoint_mute(devices_[*selected].id, !state.muted);
            status_ = devices_[*selected].name + (state.muted ? L" unmuted." : L" muted.");
        } catch (const std::exception& error) {
            status_ = L"Windows could not change mute: " + narrow_to_wide(error.what());
        }
        invalidate();
    }

    void start_routes() {
        if (selected_ids().empty()) {
            status_ = L"Pick at least one output device marked Ready first.";
            invalidate();
            return;
        }
        stop_routes();
        for (std::size_t index = 0; index < devices_.size(); ++index) {
            if (!selected_[index]) continue;
            const auto readiness = syncaudio::WasapiMirror::inspect_destination(devices_[index].id);
            if (!readiness.can_start) {
                selected_[index] = false;
                status_ = devices_[index].name + L" is no longer ready: " + readiness.reason;
                continue;
            }
            auto route = std::make_unique<RouteWorker>(window_, devices_[index].id, devices_[index].name);
            route->start();
            routes_.push_back(std::move(route));
        }
        status_ = routes_.empty() ? L"No output could be started. Refresh after reconnecting the device." : L"Starting " + std::to_wstring(routes_.size()) + L" real audio route(s)...";
        invalidate();
    }

    void stop_routes() {
        for (auto& route : routes_) route->stop();
        routes_.clear();
    }

    void draw_brand(HDC context, const Rect& header) const {
        const int icon_x = header.left + dp(26), icon_y = header.top + dp(30);
        draw_disc(context, {icon_x, icon_y, icon_x + dp(55), icon_y + dp(55)}, accent_soft, accent);
        HPEN pen = CreatePen(PS_SOLID, dp(3), accent); const HGDIOBJ previous = SelectObject(context, pen);
        Arc(context, icon_x + dp(12), icon_y + dp(14), icon_x + dp(42), icon_y + dp(44), icon_x + dp(27), icon_y + dp(14), icon_x + dp(27), icon_y + dp(44));
        Arc(context, icon_x + dp(18), icon_y + dp(20), icon_x + dp(36), icon_y + dp(38), icon_x + dp(27), icon_y + dp(20), icon_x + dp(27), icon_y + dp(38));
        SelectObject(context, previous); DeleteObject(pen);
        draw_text(context, L"SyncAudio", Rect{icon_x + dp(72), header.top + dp(25), header.right - dp(290), header.top + dp(65)}.native(), title_font_, ink, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        draw_text(context, L"A simple shared listening space for every connected output.", Rect{icon_x + dp(73), header.top + dp(70), header.right - dp(290), header.top + dp(98)}.native(), body_font_, muted_ink, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        const int count = selected_count();
        const Rect badge{header.right - dp(202), header.top + dp(39), header.right - dp(28), header.top + dp(88)};
        draw_disc(context, badge, count ? accent_soft : panel, count ? accent : shadow);
        draw_text(context, std::to_wstring(count) + L" of 5 in group", badge.native(), body_font_, count ? accent : muted_ink, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    void draw_device_area(HDC context, const Layout& page) const {
        draw_text(context, L"Pick your listening devices", Rect{page.device_area.left + dp(24), page.device_area.top + dp(20), page.refresh.left - dp(12), page.device_area.top + dp(54)}.native(), heading_font_, ink, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        draw_text(context, L"Tap a card to add it. Only outputs Windows can route right now are selectable.", Rect{page.device_area.left + dp(24), page.device_area.top + dp(49), page.refresh.left - dp(12), page.device_area.top + dp(77)}.native(), small_font_, muted_ink, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        draw_button(context, page.refresh, L"Refresh", false, false, false);
        SaveDC(context);
        const RECT clip = page.device_viewport.native();
        IntersectClipRect(context, clip.left, clip.top, clip.right, clip.bottom);
        if (devices_.empty()) draw_text(context, L"No audio outputs found", page.device_viewport.native(), heading_font_, muted_ink, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        for (std::size_t index = 0; index < devices_.size(); ++index) draw_device_card(context, index, card_rect(index, page));
        RestoreDC(context, -1);
    }

    void draw_device_card(HDC context, std::size_t index, const Rect& card) const {
        const auto viewport = layout().device_viewport;
        if (card.bottom < viewport.top || card.top > viewport.bottom) return;
        const bool focused = selected_id_ && *selected_id_ == static_cast<int>(index);
        const bool grouped = selected_[index], ready = readiness_[index].can_start;
        draw_panel(context, card, dp(18), grouped ? panel_selected : panel, focused ? accent : shadow);
        const Rect icon{card.left + dp(18), card.top + dp(22), card.left + dp(72), card.top + dp(76)};
        draw_disc(context, icon, ready ? accent_soft : RGB(234, 235, 242), ready ? accent : disabled);
        draw_device_icon(context, icon, devices_[index].name, ready ? accent : disabled);
        draw_check(context, {card.right - dp(44), card.top + dp(18), card.right - dp(20), card.top + dp(42)}, grouped, ready);
        draw_text(context, devices_[index].name.empty() ? L"Unnamed output" : devices_[index].name, Rect{card.left + dp(88), card.top + dp(18), card.right - dp(54), card.top + dp(47)}.native(), body_font_, ink, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        const std::wstring format = devices_[index].sample_rate == 0 ? L"No active format" : std::to_wstring(devices_[index].sample_rate / 1000) + L" kHz  •  " + std::to_wstring(devices_[index].channels) + L" channels";
        draw_text(context, format, Rect{card.left + dp(88), card.top + dp(49), card.right - dp(18), card.top + dp(72)}.native(), small_font_, muted_ink, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        const std::wstring state = devices_[index].is_default_multimedia ? L"SYSTEM SOURCE" : (ready ? L"READY TO SHARE" : L"UNAVAILABLE");
        const COLORREF colour = devices_[index].is_default_multimedia ? accent : (ready ? good : warning);
        draw_status_dot(context, {card.left + dp(19), card.top + dp(88), card.left + dp(29), card.top + dp(98)}, colour);
        draw_text(context, state, Rect{card.left + dp(36), card.top + dp(81), card.right - dp(18), card.bottom - dp(12)}.native(), small_font_, colour, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    void draw_control_deck(HDC context, const Layout& page) const {
        draw_text(context, L"Control deck", Rect{page.control.left + dp(24), page.control.top + dp(20), page.control.right - dp(24), page.control.top + dp(52)}.native(), heading_font_, ink, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        const auto selected = selected_device_index();
        if (!selected) {
            draw_text(context, L"Select any device card", Rect{page.control.left + dp(24), page.control.top + dp(92), page.control.right - dp(24), page.control.top + dp(130)}.native(), body_font_, muted_ink, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            return;
        }
        const auto& device = devices_[*selected];
        const bool ready = readiness_[*selected].can_start;
        const Rect icon{page.control.left + dp(24), page.control.top + dp(76), page.control.left + dp(86), page.control.top + dp(138)};
        draw_disc(context, icon, ready ? accent_soft : RGB(235, 236, 242), ready ? accent : disabled);
        draw_device_icon(context, icon, device.name, ready ? accent : disabled);
        draw_text(context, device.name.empty() ? L"Unnamed output" : device.name, Rect{page.control.left + dp(102), page.control.top + dp(80), page.control.right - dp(24), page.control.top + dp(110)}.native(), body_font_, ink, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        draw_text(context, ready ? L"Ready to add to the group" : readiness_[*selected].reason, Rect{page.control.left + dp(102), page.control.top + dp(110), page.control.right - dp(24), page.control.top + dp(146)}.native(), small_font_, ready ? good : warning, DT_LEFT | DT_WORDBREAK);
        draw_text(context, L"Device volume", Rect{page.control.left + dp(24), page.control.top + dp(175), page.control.right - dp(24), page.control.top + dp(202)}.native(), body_font_, ink, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        syncaudio::EndpointVolumeState volume{}; bool can_control = false;
        try { volume = syncaudio::endpoint_volume_state(device.id); can_control = true; } catch (const std::exception&) {}
        const std::wstring percentage = can_control ? std::to_wstring(static_cast<int>(volume.scalar * 100.0F + 0.5F)) + L"%" : L"Unavailable";
        draw_text(context, percentage, Rect{page.control.right - dp(96), page.control.top + dp(175), page.control.right - dp(24), page.control.top + dp(202)}.native(), body_font_, can_control ? accent : muted_ink, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        draw_slider(context, page.slider, can_control ? volume.scalar : 0.0F, can_control);
        draw_button(context, page.mute, can_control ? (volume.muted ? L"Unmute" : L"Mute") : L"Mute", false, !can_control, false);
        const bool has_group = selected_count() > 0;
        draw_button(context, page.start, routes_.empty() ? L"Start audio sharing" : L"Audio sharing active", true, !has_group || !routes_.empty(), !routes_.empty());
        draw_button(context, page.stop, L"Stop audio sharing", false, routes_.empty(), false);
        const Rect message{page.control.left + dp(24), page.control.bottom - dp(245), page.control.right - dp(24), page.control.bottom - dp(178)};
        draw_panel(context, message, dp(14), accent_soft, accent_soft);
        draw_text(context, L"Group status", Rect{message.left + dp(15), message.top + dp(11), message.right - dp(15), message.top + dp(31)}.native(), small_font_, accent, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        const std::wstring group_text = routes_.empty() ? L"Your group is ready when every chosen device says Ready." : std::to_wstring(routes_.size()) + L" outputs are receiving routed system audio.";
        draw_text(context, group_text, Rect{message.left + dp(15), message.top + dp(31), message.right - dp(15), message.bottom - dp(9)}.native(), small_font_, ink, DT_LEFT | DT_WORDBREAK);
        draw_text(context, status_, Rect{page.control.left + dp(24), page.control.bottom - dp(38), page.control.right - dp(24), page.control.bottom - dp(14)}.native(), small_font_, muted_ink, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    static void fill_rect(HDC context, const RECT& rectangle, COLORREF colour) {
        HBRUSH brush = CreateSolidBrush(colour); FillRect(context, &rectangle, brush); DeleteObject(brush);
    }

    void draw_panel(HDC context, const Rect& rectangle, int radius, COLORREF fill = panel, COLORREF border = shadow) const {
        HBRUSH shade = CreateSolidBrush(shadow); HPEN none = CreatePen(PS_NULL, 0, shadow);
        HGDIOBJ old_brush = SelectObject(context, shade), old_pen = SelectObject(context, none);
        RoundRect(context, rectangle.left + dp(4), rectangle.top + dp(5), rectangle.right + dp(4), rectangle.bottom + dp(5), radius, radius);
        SelectObject(context, old_brush); SelectObject(context, old_pen); DeleteObject(shade); DeleteObject(none);
        HBRUSH body = CreateSolidBrush(fill); HPEN edge = CreatePen(PS_SOLID, 1, border);
        old_brush = SelectObject(context, body); old_pen = SelectObject(context, edge);
        RoundRect(context, rectangle.left, rectangle.top, rectangle.right, rectangle.bottom, radius, radius);
        SelectObject(context, old_brush); SelectObject(context, old_pen); DeleteObject(body); DeleteObject(edge);
        HPEN light = CreatePen(PS_SOLID, 1, highlight); old_pen = SelectObject(context, light);
        Arc(context, rectangle.left + dp(2), rectangle.top + dp(1), rectangle.right - dp(2), rectangle.bottom - dp(1), rectangle.left + radius, rectangle.top, rectangle.right - radius, rectangle.top);
        SelectObject(context, old_pen); DeleteObject(light);
    }

    static void draw_text(HDC context, const std::wstring& text, RECT rectangle, HFONT font, COLORREF colour, UINT format) {
        SetBkMode(context, TRANSPARENT); SetTextColor(context, colour);
        const HGDIOBJ old = SelectObject(context, font);
        DrawTextW(context, text.c_str(), static_cast<int>(text.size()), &rectangle, format);
        SelectObject(context, old);
    }

    static void draw_disc(HDC context, const Rect& rectangle, COLORREF fill, COLORREF border) {
        HBRUSH brush = CreateSolidBrush(fill); HPEN pen = CreatePen(PS_SOLID, 1, border);
        HGDIOBJ old_brush = SelectObject(context, brush), old_pen = SelectObject(context, pen);
        Ellipse(context, rectangle.left, rectangle.top, rectangle.right, rectangle.bottom);
        SelectObject(context, old_brush); SelectObject(context, old_pen); DeleteObject(brush); DeleteObject(pen);
    }

    void draw_button(HDC context, const Rect& rectangle, const std::wstring& text, bool primary, bool is_disabled, bool is_active) const {
        const COLORREF fill = is_disabled ? RGB(227, 229, 239) : primary ? accent : panel;
        const COLORREF edge = is_disabled ? RGB(221, 223, 234) : primary ? accent : shadow;
        draw_panel(context, rectangle, dp(16), fill, edge);
        draw_text(context, text, rectangle.native(), body_font_, is_disabled ? disabled : primary ? highlight : ink, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        if (is_active) draw_status_dot(context, {rectangle.left + dp(18), rectangle.top + rectangle.height() / 2 - dp(5), rectangle.left + dp(28), rectangle.top + rectangle.height() / 2 + dp(5)}, highlight);
    }

    void draw_slider(HDC context, const Rect& rectangle, float value, bool enabled) const {
        const int center = (rectangle.top + rectangle.bottom) / 2;
        HPEN track = CreatePen(PS_SOLID, dp(8), RGB(220, 222, 235)); HGDIOBJ old = SelectObject(context, track);
        MoveToEx(context, rectangle.left, center, nullptr); LineTo(context, rectangle.right, center); SelectObject(context, old); DeleteObject(track);
        if (!enabled) return;
        const int position = rectangle.left + static_cast<int>(value * rectangle.width());
        HPEN active = CreatePen(PS_SOLID, dp(8), accent); old = SelectObject(context, active);
        MoveToEx(context, rectangle.left, center, nullptr); LineTo(context, position, center); SelectObject(context, old); DeleteObject(active);
        draw_disc(context, {position - dp(12), center - dp(12), position + dp(12), center + dp(12)}, highlight, accent);
    }

    void draw_check(HDC context, const Rect& rectangle, bool checked, bool ready) const {
        const COLORREF fill = checked ? accent : panel, edge = checked ? accent : ready ? muted_ink : disabled;
        HBRUSH brush = CreateSolidBrush(fill); HPEN pen = CreatePen(PS_SOLID, dp(2), edge);
        HGDIOBJ old_brush = SelectObject(context, brush), old_pen = SelectObject(context, pen);
        RoundRect(context, rectangle.left, rectangle.top, rectangle.right, rectangle.bottom, dp(7), dp(7));
        SelectObject(context, old_brush); SelectObject(context, old_pen); DeleteObject(brush); DeleteObject(pen);
        if (checked) {
            HPEN tick = CreatePen(PS_SOLID, dp(2), highlight); old_pen = SelectObject(context, tick);
            MoveToEx(context, rectangle.left + dp(6), rectangle.top + dp(12), nullptr);
            LineTo(context, rectangle.left + dp(10), rectangle.top + dp(16));
            LineTo(context, rectangle.right - dp(5), rectangle.top + dp(7));
            SelectObject(context, old_pen); DeleteObject(tick);
        }
    }

    static void draw_status_dot(HDC context, const Rect& rectangle, COLORREF colour) {
        HBRUSH brush = CreateSolidBrush(colour); const HGDIOBJ old = SelectObject(context, brush);
        Ellipse(context, rectangle.left, rectangle.top, rectangle.right, rectangle.bottom);
        SelectObject(context, old); DeleteObject(brush);
    }

    void draw_device_icon(HDC context, const Rect& rectangle, const std::wstring& name, COLORREF colour) const {
        const std::wstring lower = lower_case(name); HPEN pen = CreatePen(PS_SOLID, dp(2), colour); const HGDIOBJ old = SelectObject(context, pen);
        if (lower.find(L"head") != std::wstring::npos || lower.find(L"buds") != std::wstring::npos || lower.find(L"air") != std::wstring::npos) {
            Arc(context, rectangle.left + dp(12), rectangle.top + dp(10), rectangle.right - dp(12), rectangle.bottom - dp(8), rectangle.left + dp(16), rectangle.top + dp(20), rectangle.right - dp(16), rectangle.top + dp(20));
            Rectangle(context, rectangle.left + dp(10), rectangle.top + dp(29), rectangle.left + dp(18), rectangle.top + dp(43));
            Rectangle(context, rectangle.right - dp(18), rectangle.top + dp(29), rectangle.right - dp(10), rectangle.top + dp(43));
        } else {
            Rectangle(context, rectangle.left + dp(15), rectangle.top + dp(25), rectangle.left + dp(27), rectangle.top + dp(40));
            POINT speaker[] = {{rectangle.left + dp(27), rectangle.top + dp(25)}, {rectangle.left + dp(38), rectangle.top + dp(16)}, {rectangle.left + dp(38), rectangle.top + dp(49)}, {rectangle.left + dp(27), rectangle.top + dp(40)}};
            Polygon(context, speaker, 4);
            Arc(context, rectangle.left + dp(30), rectangle.top + dp(20), rectangle.right - dp(8), rectangle.bottom - dp(15), rectangle.right - dp(9), rectangle.top + dp(26), rectangle.right - dp(9), rectangle.bottom - dp(21));
        }
        SelectObject(context, old); DeleteObject(pen);
    }

    [[nodiscard]] static std::wstring lower_case(std::wstring value) {
        std::transform(value.begin(), value.end(), value.begin(), [](wchar_t character) { return static_cast<wchar_t>(towlower(character)); });
        return value;
    }

    [[nodiscard]] static std::wstring narrow_to_wide(const char* text) {
        if (text == nullptr) return L"Unknown error.";
        return std::wstring(text, text + std::strlen(text));
    }

    void invalidate() const { InvalidateRect(window_, nullptr, FALSE); }

    HWND window_{};
    UINT dpi_{96};
    HFONT title_font_{};
    HFONT heading_font_{};
    HFONT body_font_{};
    HFONT small_font_{};
    std::vector<syncaudio::DeviceDescriptor> devices_;
    std::vector<syncaudio::RouteReadiness> readiness_;
    std::vector<bool> selected_;
    std::optional<int> selected_id_;
    std::vector<std::unique_ptr<RouteWorker>> routes_;
    std::wstring status_{L"Looking for connected audio outputs..."};
    int scroll_y_{0};
    bool dragging_volume_{false};
};

Dashboard* dashboard(HWND window) { return reinterpret_cast<Dashboard*>(GetWindowLongPtrW(window, GWLP_USERDATA)); }

LRESULT CALLBACK window_procedure(HWND window, UINT message, WPARAM w_param, LPARAM l_param) {
    Dashboard* view = dashboard(window);
    switch (message) {
    case WM_NCCREATE:
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(new Dashboard(window)));
        return TRUE;
    case WM_SIZE:
        if (view) view->on_size();
        return 0;
    case WM_DPICHANGED:
        if (view) view->on_dpi_changed(HIWORD(w_param), *reinterpret_cast<RECT*>(l_param));
        return 0;
    case WM_GETMINMAXINFO: {
        const UINT dpi = GetDpiForWindow(window);
        reinterpret_cast<MINMAXINFO*>(l_param)->ptMinTrackSize = {MulDiv(1000, static_cast<int>(dpi), 96), MulDiv(680, static_cast<int>(dpi), 96)};
        return 0;
    }
    case WM_TIMER:
        if (view && w_param == device_refresh_timer) view->on_timer();
        return 0;
    case WM_MOUSEWHEEL:
        if (view) {
            POINT point{GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param)};
            ScreenToClient(window, &point);
            view->on_mouse_wheel(GET_WHEEL_DELTA_WPARAM(w_param), point);
        }
        return 0;
    case WM_LBUTTONDOWN:
        if (view) view->on_left_down({GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param)});
        return 0;
    case WM_MOUSEMOVE:
        if (view) view->on_mouse_move({GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param)});
        return 0;
    case WM_LBUTTONUP:
        if (view) view->on_left_up();
        return 0;
    case message_route_status:
        if (view) view->route_status(reinterpret_cast<std::wstring*>(l_param));
        else delete reinterpret_cast<std::wstring*>(l_param);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        if (view) {
            PAINTSTRUCT paint{}; HDC context = BeginPaint(window, &paint);
            view->paint(context);
            EndPaint(window, &paint);
            return 0;
        }
        break;
    case WM_NCDESTROY:
        delete view;
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
    WNDCLASSW window_class{};
    window_class.hInstance = instance;
    window_class.lpszClassName = window_class_name;
    window_class.lpfnWndProc = window_procedure;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hbrBackground = nullptr;
    RegisterClassW(&window_class);
    HWND window = CreateWindowExW(0, window_class_name, L"SyncAudio", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                  CW_USEDEFAULT, CW_USEDEFAULT, 1240, 800, nullptr, nullptr, instance, nullptr);
    const BOOL dark = FALSE;
    DwmSetWindowAttribute(window, 20, &dark, sizeof(dark));
    const int backdrop = 2;
    DwmSetWindowAttribute(window, 38, &backdrop, sizeof(backdrop));
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
