#include "receiver/app.hpp"
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <d3d11.h>
#include <dxgi.h>
#include <fstream>
#include <future>
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>
#include <sstream>
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
namespace {
ID3D11Device* device = nullptr;
ID3D11DeviceContext* context = nullptr;
IDXGISwapChain* swapchain = nullptr;
ID3D11RenderTargetView* target = nullptr;
void release_target() {
    if (target) {
        target->Release();
        target = nullptr;
    }
}
void create_target() {
    ID3D11Texture2D* back = nullptr;
    if (SUCCEEDED(swapchain->GetBuffer(0, IID_PPV_ARGS(&back)))) {
        device->CreateRenderTargetView(back, nullptr, &target);
        back->Release();
    }
}
LRESULT WINAPI window_proc(HWND window, UINT message, WPARAM w, LPARAM l) {
    if (ImGui_ImplWin32_WndProcHandler(window, message, w, l))
        return true;
    if (message == WM_SIZE && device && w != SIZE_MINIMIZED) {
        release_target();
        swapchain->ResizeBuffers(0, LOWORD(l), HIWORD(l), DXGI_FORMAT_UNKNOWN, 0);
        create_target();
        return 0;
    }
    if (message == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, w, l);
}
bool input_string(const char* label, std::string& value) {
    std::array<char, 1024> b{};
    std::snprintf(b.data(), b.size(), "%s", value.c_str());
    if (ImGui::InputText(label, b.data(), b.size())) {
        value = b.data();
        return true;
    }
    return false;
}
struct Texture {
    ID3D11Texture2D* image = nullptr;
    ID3D11ShaderResourceView* view = nullptr;
    int w = 0, h = 0;
    uint64_t session = 0;
    uint32_t sequence = 0;
    std::vector<uint8_t> rgba;
    ~Texture() {
        clear();
    }
    void clear() {
        if (view)
            view->Release();
        if (image)
            image->Release();
        view = nullptr;
        image = nullptr;
        w = h = 0;
        session = 0;
        rgba.clear();
    }
    void update(const receiver::Frame& f) {
        if (view && session == f.header.session && sequence == f.header.sequence)
            return;
        if (w != f.header.width || h != f.header.height) {
            clear();
            w = f.header.width;
            h = f.header.height;
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = w;
            desc.Height = h;
            desc.MipLevels = desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            if (FAILED(device->CreateTexture2D(&desc, nullptr, &image)) ||
                FAILED(device->CreateShaderResourceView(image, nullptr, &view))) {
                clear();
                return;
            }
            rgba.resize(size_t(w) * h * 4);
        }
        int c = f.header.format == 1 ? 3 : 4;
        for (size_t i = 0; i < size_t(w) * h; ++i) {
            rgba[i * 4] = f.pixels[i * c + (c == 4 ? 2 : 0)];
            rgba[i * 4 + 1] = f.pixels[i * c + 1];
            rgba[i * 4 + 2] = f.pixels[i * c + (c == 4 ? 0 : 2)];
            rgba[i * 4 + 3] = 255;
        }
        context->UpdateSubresource(image, 0, nullptr, rgba.data(), UINT(w * 4), 0);
        session = f.header.session;
        sequence = f.header.sequence;
    }
};
void save_backbuffer(const char* path) {
    ID3D11Texture2D* back = nullptr;
    ID3D11Texture2D* staging = nullptr;
    if (FAILED(swapchain->GetBuffer(0, IID_PPV_ARGS(&back))))
        throw std::runtime_error("Cannot read GUI framebuffer");
    D3D11_TEXTURE2D_DESC desc{};
    back->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &staging))) {
        back->Release();
        throw std::runtime_error("Cannot stage GUI framebuffer");
    }
    context->CopyResource(staging, back);
    back->Release();
    D3D11_MAPPED_SUBRESOURCE map{};
    if (FAILED(context->Map(staging, 0, D3D11_MAP_READ, 0, &map))) {
        staging->Release();
        throw std::runtime_error("Cannot map GUI framebuffer");
    }
    BITMAPFILEHEADER file{};
    file.bfType = 0x4d42;
    file.bfOffBits = sizeof(file) + sizeof(BITMAPINFOHEADER);
    file.bfSize = file.bfOffBits + desc.Width * desc.Height * 4;
    BITMAPINFOHEADER info{};
    info.biSize = sizeof(info);
    info.biWidth = LONG(desc.Width);
    info.biHeight = -LONG(desc.Height);
    info.biPlanes = 1;
    info.biBitCount = 32;
    info.biCompression = BI_RGB;
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(&file), sizeof(file));
    out.write(reinterpret_cast<const char*>(&info), sizeof(info));
    std::vector<uint8_t> row(desc.Width * 4);
    for (UINT y = 0; y < desc.Height; ++y) {
        auto* src = static_cast<const uint8_t*>(map.pData) + y * map.RowPitch;
        for (UINT x = 0; x < desc.Width; ++x) {
            row[4 * x] = src[4 * x + 2];
            row[4 * x + 1] = src[4 * x + 1];
            row[4 * x + 2] = src[4 * x];
            row[4 * x + 3] = 255;
        }
        out.write(reinterpret_cast<const char*>(row.data()), std::streamsize(row.size()));
    }
    context->Unmap(staging, 0);
    staging->Release();
    if (!out)
        throw std::runtime_error("Cannot save GUI smoke-test image");
}
} // namespace
int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR arguments, int) {
    bool smoke = std::string(arguments).find("--smoke-test") != std::string::npos;
    WNDCLASSEXW wc{sizeof(wc), CS_CLASSDC, window_proc,          0,      0, instance, nullptr, nullptr,
                   nullptr,    nullptr,    L"UdpVisionReceiver", nullptr};
    RegisterClassExW(&wc);
    HWND window = CreateWindowW(wc.lpszClassName, L"UDP Vision Receiver", WS_OVERLAPPEDWINDOW, 100, 100, 1240,
                                900, nullptr, nullptr, instance, nullptr);
    DXGI_SWAP_CHAIN_DESC desc{};
    desc.BufferCount = 2;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow = window;
    desc.SampleDesc.Count = 1;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    D3D_FEATURE_LEVEL feature;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0};
    if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 1,
                                             D3D11_SDK_VERSION, &desc, &swapchain, &device, &feature,
                                             &context))) {
        MessageBoxW(window, L"DirectX 11 initialization failed", L"Receiver", MB_ICONERROR);
        DestroyWindow(window);
        UnregisterClassW(wc.lpszClassName, instance);
        return 1;
    }
    create_target();
    ShowWindow(window, smoke ? SW_HIDE : SW_SHOWDEFAULT);
    UpdateWindow(window);
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::GetIO().Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 16);
    ImGui::StyleColorsDark();
    auto& style = ImGui::GetStyle();
    style.WindowRounding = 6;
    style.FrameRounding = 4;
    style.ItemSpacing = ImVec2(8, 8);
    ImGui_ImplWin32_Init(window);
    ImGui_ImplDX11_Init(device, context);
    receiver::App app;
    receiver::Settings settings;
    bool simulate = false, done = false;
    std::string profile = "profiles/default.json", error, class_text;
    std::future<void> operation;
    Texture texture;
    std::array<float, 180> latency{};
    size_t graph = 0;
    auto smoke_started = receiver::now_ns();
    int smoke_exit = 0;
    if (smoke) {
        simulate = true;
        settings.preview = true;
        app.start(settings, true);
    }
    auto busy = [&]() {
        return operation.valid() && operation.wait_for(std::chrono::seconds(0)) != std::future_status::ready;
    };
    while (!done) {
        auto ui_start = receiver::now_ns();
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
            if (message.message == WM_QUIT)
                done = true;
        }
        if (done)
            break;
        if (operation.valid() && !busy()) {
            try {
                operation.get();
                error.clear();
            } catch (const std::exception& e) {
                error = e.what();
            }
        }
        auto stats = app.stats();
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::Begin("Receiver", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoSavedSettings);
        ImGui::Text("UDP VISION RECEIVER");
        ImGui::SameLine();
        ImGui::TextDisabled("Windows / YOLO11 / Raspberry Pi");
        ImGui::TextWrapped("%s", stats.status.c_str());
        ImGui::TextDisabled("%s", stats.backend.c_str());
        if (!stats.error.empty())
            ImGui::TextColored(ImVec4(1, .4f, .3f, 1), "%s", stats.error.c_str());
        if (!error.empty())
            ImGui::TextWrapped("%s", error.c_str());
        ImGui::BeginDisabled(busy());
        if (!stats.running) {
            if (ImGui::Button("Start receiver")) {
                auto cfg = settings;
                operation = std::async(std::launch::async, [&, cfg] { app.start(cfg, simulate); });
            }
        } else {
            if (ImGui::Button("Stop")) {
                app.arm(false);
                operation = std::async(std::launch::async, [&] { app.stop(); });
            }
            ImGui::SameLine();
            bool armed = stats.armed;
            if (ImGui::Checkbox("Arm mouse output", &armed))
                app.arm(armed);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("EMERGENCY STOP (Delete)"))
            app.arm(false);
        if (busy())
            ImGui::TextDisabled("Changing pipeline state; engine builds may take several minutes.");
        ImGui::Separator();
        ImGui::BeginChild("Controls", ImVec2(470, 0), ImGuiChildFlags_Borders);
        ImGui::PushItemWidth(175);
        ImGui::BeginDisabled(stats.running || busy());
        if (ImGui::CollapsingHeader("Network and model", ImGuiTreeNodeFlags_DefaultOpen)) {
            input_string("Listen IPv4", settings.bind_ip);
            ImGui::InputInt("Frame port", &settings.frame_port);
            input_string("Sender IPv4", settings.sender_ip);
            input_string("Pi IPv4", settings.pi_ip);
            ImGui::InputInt("Pi port", &settings.pi_port);
            input_string("ONNX model", settings.model);
            input_string("Manifest (optional)", settings.metadata);
            ImGui::InputInt("Model input size", &settings.input_size, 32, 160);
            ImGui::Checkbox("Loopback simulation (no CUDA)", &simulate);
            ImGui::TextWrapped("Simulation uses a fixed test detection. Both IPs must be 127.0.0.1. "
                               "Model/network changes require restart.");
        }
        ImGui::EndDisabled();
        bool changed = false;
        if (ImGui::CollapsingHeader("Detection", ImGuiTreeNodeFlags_DefaultOpen)) {
            changed |= ImGui::SliderFloat("Confidence", &settings.confidence, 0, 1);
            changed |= ImGui::SliderFloat("NMS overlap", &settings.nms_iou, 0, 1);
            changed |= ImGui::SliderFloat("FOV radius (capture px)", &settings.fov_radius, 0, 1024);
            changed |= ImGui::InputFloat("Reference X (-1 = center)", &settings.reference_x);
            changed |= ImGui::InputFloat("Reference Y (-1 = center)", &settings.reference_y);
            if (input_string("Class IDs (comma separated)", class_text)) {
                try {
                    std::vector<int> ids;
                    std::stringstream text(class_text);
                    std::string part;
                    while (std::getline(text, part, ',')) {
                        size_t used = 0;
                        int id = std::stoi(part, &used);
                        if (part.find_first_not_of(" \t", used) != std::string::npos || id < 0)
                            throw std::runtime_error("Invalid class IDs");
                        ids.push_back(id);
                    }
                    settings.classes = std::move(ids);
                    changed = true;
                } catch (...) {
                    error =
                        "Enter nonnegative class IDs, separated by commas, or leave empty for all classes.";
                }
            }
            changed |= ImGui::Checkbox("Choose highest confidence", &settings.highest_confidence);
            changed |= ImGui::Checkbox("Prefer current target", &settings.persistence);
            changed |= ImGui::SliderFloat("Target match overlap", &settings.persistence_iou, .01f, 1);
            changed |= ImGui::SliderFloat("Aim X (% of box)", &settings.aim_x, 0, 1);
            changed |= ImGui::SliderFloat("Aim Y (% of box)", &settings.aim_y, 0, 1);
            changed |= ImGui::InputFloat("X offset (px)", &settings.offset_x);
            changed |= ImGui::InputFloat("Y offset (px)", &settings.offset_y);
        }
        if (ImGui::CollapsingHeader("Movement", ImGuiTreeNodeFlags_DefaultOpen)) {
            changed |= ImGui::SliderFloat("X gain (counts / px)", &settings.gain_x, 0, 2);
            changed |= ImGui::SliderFloat("Y gain (counts / px)", &settings.gain_y, 0, 2);
            changed |= ImGui::SliderFloat("Smoothing time (ms, 0 = off)", &settings.smoothing_ms, 0, 100);
            changed |= ImGui::SliderFloat("Dead zone (px)", &settings.deadzone, 0, 20);
            changed |= ImGui::SliderInt("Maximum counts / update", &settings.max_step, 1, 127);
            changed |= ImGui::SliderInt("Activation button (2 = right)", &settings.activation_button, 1, 8);
            changed |= ImGui::SliderInt("Maximum frame age (ms)", &settings.max_age_ms, 1, 250);
        }
        changed |= ImGui::Checkbox("Detection preview (30 Hz)", &settings.preview);
        if (changed) {
            try {
                receiver::validate(settings);
                if (stats.running)
                    app.configure(settings);
                error.clear();
            } catch (const std::exception& e) {
                error = e.what();
            }
        }
        if (ImGui::CollapsingHeader("Profiles", ImGuiTreeNodeFlags_DefaultOpen)) {
            input_string("Profile path", profile);
            if (ImGui::Button("Save profile")) {
                try {
                    receiver::save_settings(settings, profile);
                    error.clear();
                } catch (const std::exception& e) {
                    error = e.what();
                }
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(stats.running || busy());
            if (ImGui::Button("Load profile")) {
                try {
                    settings = receiver::load_settings(profile);
                    class_text.clear();
                    for (auto c : settings.classes) {
                        if (!class_text.empty())
                            class_text += ",";
                        class_text += std::to_string(c);
                    }
                    error.clear();
                } catch (const std::exception& e) {
                    error = e.what();
                }
            }
            ImGui::EndDisabled();
        }
        ImGui::PopItemWidth();
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("Monitor");
        ImGui::Text("Frames: %llu    Corrections: %llu", static_cast<unsigned long long>(stats.inferred),
                    static_cast<unsigned long long>(stats.sent));
        ImGui::Text("Capture: %d x %d    Physical buttons: 0x%02x", stats.width, stats.height,
                    stats.physical);
        ImGui::Text("Clock sync: %s    Uncertainty: %.3f ms", stats.synchronized ? "ready" : "waiting",
                    stats.clock_uncertainty_ms);
        ImGui::Text("Capture age upper bound: %.2f ms", stats.frame_age_ms);
        ImGui::TextDisabled("Frame pool: 28 MiB | GPU free at load: %.0f / %.0f MiB", stats.gpu_free_mib,
                            stats.gpu_total_mib);
        ImGui::Text("Expired: %llu   Replaced: %llu   Stale: %llu",
                    static_cast<unsigned long long>(stats.network.expired),
                    static_cast<unsigned long long>(stats.replaced),
                    static_cast<unsigned long long>(stats.stale));
        ImGui::Text("Invalid: %llu   Duplicates: %llu   Pool drops: %llu",
                    static_cast<unsigned long long>(stats.network.invalid),
                    static_cast<unsigned long long>(stats.network.duplicates),
                    static_cast<unsigned long long>(stats.network.pool_drops));
        if (ImGui::BeginTable("Timings", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
            for (auto label : {"Stage", "p50 ms", "p95 ms", "p99 ms"})
                ImGui::TableSetupColumn(label);
            ImGui::TableHeadersRow();
            for (auto pair : {std::pair{"Reassembly", &stats.reassembly},
                              {"Copies + preprocess", &stats.upload},
                              {"Inference", &stats.inference},
                              {"Postprocessing", &stats.postprocess},
                              {"UDP submission", &stats.submit},
                              {"Receiver to submission", &stats.receiver_total}}) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(pair.first);
                for (double p : {.5, .95, .99}) {
                    ImGui::TableNextColumn();
                    ImGui::Text("%.3f", pair.second->percentile(p));
                }
            }
            ImGui::EndTable();
        }
        latency[graph++ % latency.size()] = float(stats.receiver_total.percentile(.95));
        ImGui::PlotLines("p95 ms", latency.data(), int(latency.size()), int(graph % latency.size()), nullptr,
                         0, 50, ImVec2(0, 80));
        if (ImGui::Button("Export metrics.csv")) {
            try {
                app.export_metrics("metrics.csv");
                error.clear();
            } catch (const std::exception& e) {
                error = e.what();
            }
        }
        ImGui::TextWrapped(
            "Timings end at UDP submission. They do not prove that the target PC received USB movement.");
        if (settings.preview) {
            auto p = app.preview();
            if (p.frame) {
                texture.update(*p.frame);
                if (texture.view) {
                    float scale = std::min(2.f, std::min(ImGui::GetContentRegionAvail().x / texture.w,
                                                         ImGui::GetContentRegionAvail().y / texture.h));
                    scale = std::max(.1f, scale);
                    auto origin = ImGui::GetCursorScreenPos();
                    ImGui::Image(ImTextureID(reinterpret_cast<uintptr_t>(texture.view)),
                                 ImVec2(texture.w * scale, texture.h * scale));
                    auto* draw = ImGui::GetWindowDrawList();
                    for (auto& d : p.detections) {
                        draw->AddRect(ImVec2(origin.x + d.x * scale, origin.y + d.y * scale),
                                      ImVec2(origin.x + (d.x + d.w) * scale, origin.y + (d.y + d.h) * scale),
                                      IM_COL32(80, 230, 160, 255), 0, 0, 2);
                        char label[64];
                        std::snprintf(label, sizeof(label), "class %d %.2f", d.cls, d.score);
                        draw->AddText(ImVec2(origin.x + d.x * scale, origin.y + d.y * scale), IM_COL32_WHITE,
                                      label);
                    }
                    float rx = settings.reference_x < 0 ? texture.w * .5f : settings.reference_x,
                          ry = settings.reference_y < 0 ? texture.h * .5f : settings.reference_y;
                    draw->AddCircle(ImVec2(origin.x + rx * scale, origin.y + ry * scale),
                                    settings.fov_radius * scale, IM_COL32(180, 180, 255, 180));
                    if (p.correction.target)
                        draw->AddCircleFilled(ImVec2(origin.x + p.correction.aim_x * scale,
                                                     origin.y + p.correction.aim_y * scale),
                                              4, IM_COL32(255, 100, 80, 255));
                }
            } else
                ImGui::TextDisabled("Waiting for a frame...");
        } else
            texture.clear();
        ImGui::EndChild();
        ImGui::End();
        ImGui::Render();
        if (target) {
            const float clear[] = {.06f, .07f, .09f, 1};
            context->OMSetRenderTargets(1, &target, nullptr);
            context->ClearRenderTargetView(target, clear);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
            if (smoke && receiver::now_ns() - smoke_started > 3'000'000'000) {
                try {
                    save_backbuffer("gui-smoke.bmp");
                    auto result = app.stats();
                    if (!result.inferred || !result.pi_ready || !result.synchronized)
                        smoke_exit = 2;
                    app.export_metrics("gui-smoke-metrics.csv");
                } catch (...) {
                    smoke_exit = 3;
                }
                done = true;
            }
            swapchain->Present(0, 0);
        }
        auto remaining = 16'666'667 - (receiver::now_ns() - ui_start);
        if (remaining > 0)
            std::this_thread::sleep_for(std::chrono::nanoseconds(remaining));
    }
    if (operation.valid())
        try {
            operation.get();
        } catch (...) {
        }
    app.stop();
    texture.clear();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    release_target();
    swapchain->Release();
    context->Release();
    device->Release();
    DestroyWindow(window);
    UnregisterClassW(wc.lpszClassName, instance);
    return smoke_exit;
}
