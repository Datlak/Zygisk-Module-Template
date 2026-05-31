#include <zygisk.hpp>
#include <dlfcn.h>
#include <android/log.h>
#include <jni.h>
#include <string>
#include <vector>
#include <cmath>
#include <mutex>
#include <thread>

// ==================== IMGUI HEADERS ====================
#include "imgui.h"
#include "imgui_impl_android.h"
#include "imgui_impl_opengl3.h"

// ==================== OPENGL ES ====================
#include <EGL/egl.h>
#include <GLES3/gl3.h>

#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, "FF_ESP", __VA_ARGS__)

// ==================== OFFSETS (cập nhật theo bản game) ====================
#define OFFSET_GWORLD          0x10E2A8A0
#define OFFSET_PERSISTENTLEVEL 0x30
#define OFFSET_ACTORS          0xA0
#define OFFSET_NUMACTORS       0xAC
#define OFFSET_PLAYERSTATE     0x2F0
#define OFFSET_PLAYERNAME      0x50
#define OFFSET_HEALTH          0x6A0
#define OFFSET_ROOTCOMPONENT   0x1C0
#define OFFSET_WORLDLOCATION   0x160
#define OFFSET_CAMERACACHE     0x4B0

// ==================== CẤU HÌNH ESP ====================
struct Config {
    bool espEnabled = true;
    bool box = true;
    bool healthBar = true;
    bool playerName = true;
    bool distance = true;
    float maxDistance = 150.0f;
    float boxColor[4] = {0.0f, 1.0f, 0.0f, 1.0f};
    float textColor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
} g_config;

struct Vector3 { float x, y, z; };
struct Player {
    std::string name;
    Vector3 pos;
    float health;
    float distance;
    float screenX, screenY;
    bool visible;
};

class FreeFireESP : public zygisk::ModuleBase {
public:
    void onLoad(zygisk::Api *api, JNIEnv *env) override {
        this->api = api;
        this->env = env;
    }
    
    void preAppSpecialize(zygisk::AppSpecializeArgs *args) override {
        const char *pkg = env->GetStringUTFChars(args->nice_name, nullptr);
        if (strcmp(pkg, "com.dts.freefireth") == 0) {
            api->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY);
            isFreeFire = true;
        }
        env->ReleaseStringUTFChars(args->nice_name, pkg);
    }
    
    void postAppSpecialize(const zygisk::AppSpecializeArgs*) override {
        if (!isFreeFire) return;
        
        // Chờ game load
        while (!libUE4) {
            libUE4 = dlopen("libUE4.so", RTLD_NOLOAD);
            sleep(1);
        }
        base = (uintptr_t)libUE4;
        LOGD("libUE4 base: 0x%lx", base);
        
        // Khởi tạo ImGui
        setupImGui();
        
        // Hook hàm vẽ
        hookDraw();
        
        // Bắt đầu scan player
        startScanning();
    }
    
private:
    zygisk::Api *api = nullptr;
    JNIEnv *env = nullptr;
    bool isFreeFire = false;
    void *libUE4 = nullptr;
    uintptr_t base = 0;
    std::vector<Player> players;
    std::mutex playerMutex;
    ANativeWindow* gameWindow = nullptr;
    bool initialized = false;
    
    // Matrix cho World to Screen (cần lấy từ game)
    float viewMatrix[16] = {0};
    float projMatrix[16] = {0};
    
    void setupImGui() {
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        
        // Style
        ImGuiStyle& style = ImGui::GetStyle();
        style.WindowRounding = 5.0f;
        style.Colors[ImGuiCol_WindowBg] = ImVec4(0.1f, 0.1f, 0.1f, 0.8f);
        
        LOGD("ImGui context created");
    }
    
    void initImGuiForWindow(ANativeWindow* window) {
        if (!window) return;
        gameWindow = window;
        ImGui_ImplAndroid_Init(window);
        ImGui_ImplOpenGL3_Init("#version 300 es");
        initialized = true;
        LOGD("ImGui initialized for window");
    }
    
    void hookDraw() {
        // Hook UGameViewportClient::Draw hoặc hàm tương tự
        // Tạm thời dùng thread vẽ riêng (cần hook thực tế)
        std::thread([this]() {
            while (true) {
                if (initialized && gameWindow) {
                    drawMenuAndESP();
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
            }
        }).detach();
    }
    
    void drawMenuAndESP() {
        if (!initialized) return;
        
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplAndroid_NewFrame();
        ImGui::NewFrame();
        
        // ========== MENU CHÍNH ==========
        ImGui::Begin("FreeFire ESP v3.0", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
        
        ImGui::Checkbox("ESP Master", &g_config.espEnabled);
        ImGui::Separator();
        
        if (ImGui::CollapsingHeader("ESP Options", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Checkbox("Box ESP", &g_config.box);
            ImGui::Checkbox("Health Bar", &g_config.healthBar);
            ImGui::Checkbox("Player Name", &g_config.playerName);
            ImGui::Checkbox("Distance", &g_config.distance);
            ImGui::SliderFloat("Max Distance", &g_config.maxDistance, 20.0f, 300.0f);
            ImGui::ColorEdit4("Box Color", g_config.boxColor);
        }
        
        ImGui::Text("Players: %d", (int)players.size());
        ImGui::Text("FPS: %.1f", ImGui::GetIO().Framerate);
        
        if (ImGui::Button("Exit Menu")) {
            // Ẩn menu
        }
        
        ImGui::End();
        
        // ========== VẼ ESP ==========
        if (g_config.espEnabled) {
            drawESP();
        }
        
        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    }
    
    void drawESP() {
        std::lock_guard<std::mutex> lock(playerMutex);
        ImDrawList* draw = ImGui::GetBackgroundDrawList();
        float screenW = ImGui::GetIO().DisplaySize.x;
        float screenH = ImGui::GetIO().DisplaySize.y;
        
        for (const auto& p : players) {
            if (p.distance > g_config.maxDistance) continue;
            if (!worldToScreen(p.pos.x, p.pos.y, p.pos.z, screenW, screenH, p.screenX, p.screenY)) continue;
            
            float boxHeight = 100.0f * (80.0f / p.distance);
            float boxWidth = 50.0f * (80.0f / p.distance);
            float topY = p.screenY - boxHeight;
            float leftX = p.screenX - boxWidth / 2;
            
            // Box ESP
            if (g_config.box) {
                draw->AddRect(ImVec2(leftX, topY), ImVec2(leftX + boxWidth, p.screenY),
                              IM_COL32(g_config.boxColor[0]*255, g_config.boxColor[1]*255,
                                       g_config.boxColor[2]*255, 255), 2.0f, 0, 2.0f);
            }
            
            // Health Bar
            if (g_config.healthBar) {
                float healthPercent = p.health / 100.0f;
                draw->AddRectFilled(ImVec2(leftX - 8, topY), ImVec2(leftX - 3, p.screenY),
                                    IM_COL32(0,0,0,180));
                draw->AddRectFilled(ImVec2(leftX - 7, p.screenY - boxHeight * healthPercent),
                                    ImVec2(leftX - 4, p.screenY),
                                    IM_COL32((1-healthPercent)*255, healthPercent*255, 0, 255));
            }
            
            // Player Name
            if (g_config.playerName) {
                draw->AddText(ImVec2(leftX, topY - 15),
                              IM_COL32(g_config.textColor[0]*255, g_config.textColor[1]*255,
                                       g_config.textColor[2]*255, 255),
                              p.name.c_str());
            }
            
            // Distance
            if (g_config.distance) {
                char distText[32];
                snprintf(distText, sizeof(distText), "%.0fm", p.distance);
                draw->AddText(ImVec2(leftX + boxWidth + 5, p.screenY - 10),
                              IM_COL32(255,255,255,255), distText);
            }
        }
    }
    
    bool worldToScreen(float x, float y, float z, float sw, float sh, float& sx, float& sy) {
        // Cần lấy matrix từ game, đây là placeholder
        // Thay bằng ma trận thực tế từ CameraManager
        float dx = x - cameraPos.x;
        float dy = y - cameraPos.y;
        float dz = z - cameraPos.z;
        
        float focalLength = 90.0f;
        float aspect = sw / sh;
        
        sx = sw / 2 + (dx / dz) * focalLength * aspect;
        sy = sh / 2 - (dy / dz) * focalLength;
        
        return (dz > 0.1f && sx > 0 && sx < sw && sy > 0 && sy < sh);
    }
    
    Vector3 cameraPos = {0, 0, -10};
    
    void startScanning() {
        std::thread([this]() {
            while (true) {
                scanPlayers();
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }).detach();
    }
    
    void scanPlayers() {
        uintptr_t gworld = *(uintptr_t*)(base + OFFSET_GWORLD);
        if (!gworld) return;
        
        uintptr_t level = *(uintptr_t*)(gworld + OFFSET_PERSISTENTLEVEL);
        if (!level) return;
        
        uintptr_t actorsPtr = *(uintptr_t*)(level + OFFSET_ACTORS);
        int numActors = *(int*)(level + OFFSET_NUMACTORS);
        if (numActors <= 0 || numActors > 500) return;
        
        std::vector<Player> newPlayers;
        
        for (int i = 0; i < numActors; i++) {
            uintptr_t actor = *(uintptr_t*)(actorsPtr + i * 8);
            if (!actor) continue;
            
            float health = *(float*)(actor + OFFSET_HEALTH);
            if (health <= 0.0f || health > 100.0f) continue;
            
            uintptr_t rootComp = *(uintptr_t*)(actor + OFFSET_ROOTCOMPONENT);
            if (!rootComp) continue;
            
            Player p;
            p.health = health;
            p.pos.x = *(float*)(rootComp + OFFSET_WORLDLOCATION);
            p.pos.y = *(float*)(rootComp + OFFSET_WORLDLOCATION + 4);
            p.pos.z = *(float*)(rootComp + OFFSET_WORLDLOCATION + 8);
            p.distance = sqrt(p.pos.x*p.pos.x + p.pos.y*p.pos.y + p.pos.z*p.pos.z);
            
            uintptr_t pState = *(uintptr_t*)(actor + OFFSET_PLAYERSTATE);
            if (pState) {
                const char* namePtr = (const char*)(pState + OFFSET_PLAYERNAME);
                p.name = (namePtr && namePtr[0]) ? namePtr : "Unknown";
            } else {
                p.name = "Unknown";
            }
            
            if (p.distance < 300.0f && p.distance > 0.1f) {
                newPlayers.push_back(p);
            }
        }
        
        std::lock_guard<std::mutex> lock(playerMutex);
        players = std::move(newPlayers);
    }
};

REGISTER_ZYGISK_MODULE(FreeFireESP)
