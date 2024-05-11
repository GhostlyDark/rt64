#include <Windows.h>

#include "Gfx #1.3.h"
#include "hle/rt64_application.h"
#include "queue_executor.h"

// https://github.com/Mr-Wiseguy/Zelda64Recomp/blob/b1109f66c16efd7913e816a03c5e7d259a4c6c61/ultramodern/rt64_layer.cpp
enum class Resolution {
    Original,
    Original2x,
    Auto,
    OptionCount
};
enum class WindowMode {
    Windowed,
    Fullscreen,
    OptionCount
};
enum class HUDRatioMode {
    Original,
    Clamp16x9,
    Full,
    OptionCount
};
enum class GraphicsApi {
    Auto,
    D3D12,
    Vulkan,
    OptionCount
};

struct Config {
    Resolution res_option = Resolution::Original2x;
    WindowMode wm_option = WindowMode::Windowed;
    HUDRatioMode hr_option = HUDRatioMode::Original;
    GraphicsApi api_option = GraphicsApi::D3D12;
    RT64::UserConfiguration::AspectRatio ar_option = RT64::UserConfiguration::AspectRatio::Original;
    RT64::UserConfiguration::Antialiasing msaa_option = RT64::UserConfiguration::Antialiasing::MSAA4X;
    RT64::UserConfiguration::RefreshRate rr_option = RT64::UserConfiguration::RefreshRate::Display;
    int rr_manual_value;
    int ds_option = 1;
    bool developer_mode;
};

static QueueExecutor gRSPQueue;
static std::unique_ptr<RT64::Application> gApp;
static GFX_INFO gGfxInfo;
static Config gConfig{ };
static RT64::UserConfiguration::Antialiasing device_max_msaa = RT64::UserConfiguration::Antialiasing::None;
static bool sample_positions_supported = false;

static inline void set_application_user_config(RT64::Application* application, const Config& config) {
    switch (config.res_option) {
    default:
    case Resolution::Auto:
        application->userConfig.resolution = RT64::UserConfiguration::Resolution::WindowIntegerScale;
        application->userConfig.downsampleMultiplier = 1;
        break;
    case Resolution::Original:
        application->userConfig.resolution = RT64::UserConfiguration::Resolution::Manual;
        application->userConfig.resolutionMultiplier = config.ds_option;
        application->userConfig.downsampleMultiplier = config.ds_option;
        break;
    case Resolution::Original2x:
        application->userConfig.resolution = RT64::UserConfiguration::Resolution::Manual;
        application->userConfig.resolutionMultiplier = 2.0 * config.ds_option;
        application->userConfig.downsampleMultiplier = config.ds_option;
        break;
    }

    switch (config.hr_option) {
    default:
    case HUDRatioMode::Original:
        application->userConfig.extAspectRatio = RT64::UserConfiguration::AspectRatio::Original;
        break;
    case HUDRatioMode::Clamp16x9:
        application->userConfig.extAspectRatio = RT64::UserConfiguration::AspectRatio::Manual;
        application->userConfig.extAspectTarget = 16.0 / 9.0;
        break;
    case HUDRatioMode::Full:
        application->userConfig.extAspectRatio = RT64::UserConfiguration::AspectRatio::Expand;
        break;
    }

    application->userConfig.aspectRatio = config.ar_option;
    application->userConfig.antialiasing = config.msaa_option;
    application->userConfig.refreshRate = config.rr_option;
    application->userConfig.refreshRateTarget = config.rr_manual_value;
}

static inline RT64::UserConfiguration::Antialiasing compute_max_supported_aa(RT64::RenderSampleCounts bits) {
    if (bits & RT64::RenderSampleCount::Bits::COUNT_2) {
        if (bits & RT64::RenderSampleCount::Bits::COUNT_4) {
            if (bits & RT64::RenderSampleCount::Bits::COUNT_8) {
                return RT64::UserConfiguration::Antialiasing::MSAA8X;
            }
            return RT64::UserConfiguration::Antialiasing::MSAA4X;
        }
        return RT64::UserConfiguration::Antialiasing::MSAA2X;
    };
    return RT64::UserConfiguration::Antialiasing::None;
}

static void plugin_init(void)
{    // Set up the RT64 application core fields.
    RT64::Application::Core appCore{};
    appCore.window = gGfxInfo.hWnd;

    appCore.checkInterrupts = gGfxInfo.CheckInterrupts;

    appCore.HEADER = gGfxInfo.HEADER;
    appCore.RDRAM = gGfxInfo.RDRAM;
    appCore.DMEM = gGfxInfo.DMEM;
    appCore.IMEM = gGfxInfo.IMEM;

    static_assert(sizeof(uint32_t) == sizeof(DWORD));
    appCore.MI_INTR_REG = (uint32_t*) gGfxInfo.MI_INTR_REG;

    appCore.DPC_START_REG = (uint32_t*) gGfxInfo.DPC_START_REG;
    appCore.DPC_END_REG = (uint32_t*)gGfxInfo.DPC_END_REG;
    appCore.DPC_CURRENT_REG = (uint32_t*) gGfxInfo.DPC_CURRENT_REG;
    appCore.DPC_STATUS_REG = (uint32_t*)gGfxInfo.DPC_STATUS_REG;
    appCore.DPC_CLOCK_REG = (uint32_t*)gGfxInfo.DPC_CLOCK_REG;
    appCore.DPC_BUFBUSY_REG = (uint32_t*)gGfxInfo.DPC_BUFBUSY_REG;
    appCore.DPC_PIPEBUSY_REG = (uint32_t*)gGfxInfo.DPC_PIPEBUSY_REG;
    appCore.DPC_TMEM_REG = (uint32_t*)gGfxInfo.DPC_TMEM_REG;

    appCore.VI_STATUS_REG = (uint32_t*)gGfxInfo.VI_STATUS_REG;
    appCore.VI_ORIGIN_REG = (uint32_t*)gGfxInfo.VI_ORIGIN_REG;
    appCore.VI_WIDTH_REG = (uint32_t*)gGfxInfo.VI_WIDTH_REG;
    appCore.VI_INTR_REG = (uint32_t*)gGfxInfo.VI_INTR_REG;
    appCore.VI_V_CURRENT_LINE_REG = (uint32_t*)gGfxInfo.VI_V_CURRENT_LINE_REG;
    appCore.VI_TIMING_REG = (uint32_t*)gGfxInfo.VI_TIMING_REG;
    appCore.VI_V_SYNC_REG = (uint32_t*)gGfxInfo.VI_V_SYNC_REG;
    appCore.VI_H_SYNC_REG = (uint32_t*)gGfxInfo.VI_H_SYNC_REG;
    appCore.VI_LEAP_REG = (uint32_t*)gGfxInfo.VI_LEAP_REG;
    appCore.VI_H_START_REG = (uint32_t*)gGfxInfo.VI_H_START_REG;
    appCore.VI_V_START_REG = (uint32_t*)gGfxInfo.VI_V_START_REG;
    appCore.VI_V_BURST_REG = (uint32_t*)gGfxInfo.VI_V_BURST_REG;
    appCore.VI_X_SCALE_REG = (uint32_t*)gGfxInfo.VI_X_SCALE_REG;
    appCore.VI_Y_SCALE_REG = (uint32_t*)gGfxInfo.VI_Y_SCALE_REG;

    // Set up the RT64 application configuration fields.
    RT64::ApplicationConfiguration appConfig;
    appConfig.useConfigurationFile = false;

    // Create the RT64 application.
    gApp = std::make_unique<RT64::Application>(appCore, appConfig);

    // Set initial user config settings based on the current settings.
    set_application_user_config(gApp.get(), gConfig);
    gApp->userConfig.developerMode = false;
    // Force gbi depth branches to prevent LODs from kicking in.
    gApp->enhancementConfig.f3dex.forceBranch = true;
    // Scale LODs based on the output resolution.
    gApp->enhancementConfig.textureLOD.scale = true;
    // Pick an API if the user has set an override.
    switch (gConfig.api_option) {
    case GraphicsApi::D3D12:
        gApp->userConfig.graphicsAPI = RT64::UserConfiguration::GraphicsAPI::D3D12;
        break;
    case GraphicsApi::Vulkan:
        gApp->userConfig.graphicsAPI = RT64::UserConfiguration::GraphicsAPI::Vulkan;
        break;
    default:
    case GraphicsApi::Auto:
        // Don't override if auto is selected.
        break;
    }

    // Set up the RT64 application.
    uint32_t thread_id = 0;
#ifdef _WIN32
    thread_id = GetCurrentThreadId();
#endif
    if (gApp->setup(thread_id) != RT64::Application::SetupResult::Success) {
        gApp.reset();
        return;
    }

    // Set the application's fullscreen state.
    gApp->setFullScreen(gConfig.wm_option == WindowMode::Fullscreen);

    // Check if the selected device actually supports MSAA sample positions and MSAA for for the formats that will be used
    // and downgrade the configuration accordingly.
    if (gApp->device->getCapabilities().sampleLocations) {
        RT64::RenderSampleCounts color_sample_counts = gApp->device->getSampleCountsSupported(RT64::RenderFormat::R8G8B8A8_UNORM);
        RT64::RenderSampleCounts depth_sample_counts = gApp->device->getSampleCountsSupported(RT64::RenderFormat::D32_FLOAT);
        RT64::RenderSampleCounts common_sample_counts = color_sample_counts & depth_sample_counts;
        device_max_msaa = compute_max_supported_aa(common_sample_counts);
        sample_positions_supported = true;
    }
    else {
        device_max_msaa = RT64::UserConfiguration::Antialiasing::None;
        sample_positions_supported = false;
    }
}

static void plugin_deinit(void)
{
    gApp->end();
    gApp.reset();
}

static void plugin_dl(void)
{
    auto dlistStart = *(uint32_t*)(gGfxInfo.DMEM + 0xff0);
    auto dlistSize  = *(uint32_t*)(gGfxInfo.DMEM + 0xff4);

    auto ucStart  = *(uint32_t*)(gGfxInfo.DMEM + 0xfd0);
    auto ucDStart = *(uint32_t*)(gGfxInfo.DMEM + 0xfd8);
    auto ucDSize  = *(uint32_t*)(gGfxInfo.DMEM + 0xfdc);

    gApp->state->rsp->reset();
    gApp->interpreter->loadUCodeGBI(ucStart & 0x3FFFFFF, ucDStart & 0x3FFFFFF, true);
    gApp->processDisplayLists(gGfxInfo.RDRAM, dlistStart & 0x3FFFFFF, (dlistStart + dlistSize) & 0x3FFFFFF, true);
}

static void plugin_draw(void)
{
    gApp->updateScreen();
}

BOOL APIENTRY DllMain( HMODULE hModule,
                       DWORD  ul_reason_for_call,
                       LPVOID lpReserved
                     )
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
    case DLL_PROCESS_DETACH:
        break;
    }
    return TRUE;
}

/******************************************************************
  Function: CaptureScreen
  Purpose:  This function dumps the current frame to a file
  input:    pointer to the directory to save the file to
  output:   none
*******************************************************************/
EXPORT void CALL CaptureScreen(char* Directory)
{
}

/******************************************************************
  Function: ChangeWindow
  Purpose:  to change the window between fullscreen and window
            mode. If the window was in fullscreen this should
            change the screen to window mode and vice vesa.
  input:    none
  output:   none
*******************************************************************/
EXPORT void CALL ChangeWindow(void)
{
}

/******************************************************************
  Function: CloseDLL
  Purpose:  This function is called when the emulator is closing
            down allowing the dll to de-initialise.
  input:    none
  output:   none
*******************************************************************/
EXPORT void CALL CloseDLL(void)
{

}

/******************************************************************
  Function: DllAbout
  Purpose:  This function is optional function that is provided
            to give further information about the DLL.
  input:    a handle to the window that calls this function
  output:   none
*******************************************************************/
EXPORT void CALL DllAbout(HWND hParent)
{
    
}

/******************************************************************
  Function: DllConfig
  Purpose:  This function is optional function that is provided
            to allow the user to configure the dll
  input:    a handle to the window that calls this function
  output:   none
*******************************************************************/
EXPORT void CALL DllConfig(HWND hParent)
{
}

/******************************************************************
  Function: DllTest
  Purpose:  This function is optional function that is provided
            to allow the user to test the dll
  input:    a handle to the window that calls this function
  output:   none
*******************************************************************/
EXPORT void CALL DllTest(HWND hParent)
{

}

/******************************************************************
  Function: DrawScreen
  Purpose:  This function is called when the emulator receives a
            WM_PAINT message. This allows the gfx to fit in when
            it is being used in the desktop.
  input:    none
  output:   none
*******************************************************************/
EXPORT void CALL DrawScreen(void)
{

}

/******************************************************************
  Function: GetDllInfo
  Purpose:  This function allows the emulator to gather information
            about the dll by filling in the PluginInfo structure.
  input:    a pointer to a PLUGIN_INFO stucture that needs to be
            filled by the function. (see def above)
  output:   none
*******************************************************************/
EXPORT void CALL GetDllInfo(PLUGIN_INFO* PluginInfo)
{
    PluginInfo->MemoryBswaped = true;
    strcpy(PluginInfo->Name, "RT64");
    PluginInfo->NormalMemory = true;
    PluginInfo->Type = PLUGIN_TYPE_GFX;
    PluginInfo->Version = 0x0103;
}

/******************************************************************
  Function: InitiateGFX
  Purpose:  This function is called when the DLL is started to give
            information from the emulator that the n64 graphics
            uses. This is not called from the emulation thread.
  Input:    Gfx_Info is passed to this function which is defined
            above.
  Output:   TRUE on success
            FALSE on failure to initialise

  ** note on interrupts **:
  To generate an interrupt set the appropriate bit in MI_INTR_REG
  and then call the function CheckInterrupts to tell the emulator
  that there is a waiting interrupt.
*******************************************************************/
EXPORT BOOL CALL InitiateGFX(GFX_INFO Gfx_Info)
{
    gGfxInfo = Gfx_Info;
    return TRUE;
}

/******************************************************************
  Function: MoveScreen
  Purpose:  This function is called in response to the emulator
            receiving a WM_MOVE passing the xpos and ypos passed
            from that message.
  input:    xpos - the x-coordinate of the upper-left corner of the
            client area of the window.
            ypos - y-coordinate of the upper-left corner of the
            client area of the window.
  output:   none
*******************************************************************/
EXPORT void CALL MoveScreen(int xpos, int ypos)
{

}

/******************************************************************
  Function: ProcessDList
  Purpose:  This function is called when there is a Dlist to be
            processed. (High level GFX list)
  input:    none
  output:   none
*******************************************************************/
EXPORT void CALL ProcessDList(void)
{
    gRSPQueue.sync(plugin_dl);
}

/******************************************************************
  Function: ProcessRDPList
  Purpose:  This function is called when there is a Dlist to be
            processed. (Low level GFX list)
  input:    none
  output:   none
*******************************************************************/
EXPORT void CALL ProcessRDPList(void) { }

/******************************************************************
  Function: RomClosed
  Purpose:  This function is called when a rom is closed.
  input:    none
  output:   none
*******************************************************************/
EXPORT void CALL RomClosed(void)
{
    gRSPQueue.async(plugin_deinit);
    gRSPQueue.stop();
}

/******************************************************************
  Function: RomOpen
  Purpose:  This function is called when a rom is open. (from the
            emulation thread)
  input:    none
  output:   none
*******************************************************************/
EXPORT void CALL RomOpen(void)
{
    RECT windowRect;
    GetClientRect(gGfxInfo.hWnd, &windowRect);
    RECT statusRect;
    GetWindowRect(gGfxInfo.hStatusBar, &statusRect);

    auto offset = statusRect.bottom - statusRect.top - 1;
    windowRect.right = windowRect.left + 1280 - 1;
    windowRect.bottom = windowRect.top + 960 - 1 + offset;

    AdjustWindowRect(&windowRect, GetWindowLong(gGfxInfo.hWnd, GWL_STYLE), GetMenu(gGfxInfo.hWnd) != NULL);

    SetWindowPos(gGfxInfo.hWnd, NULL, 0, 0, windowRect.right - windowRect.left + 1,
        windowRect.bottom - windowRect.top + 2, SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOMOVE);

    // TODO: refresh config
    gRSPQueue.start(true /*sameThreadExec*/);
    gRSPQueue.async(plugin_init);
}

/******************************************************************
  Function: ShowCFB
  Purpose:  Useally once Dlists are started being displayed, cfb is
            ignored. This function tells the dll to start displaying
            them again.
  input:    none
  output:   none
*******************************************************************/
EXPORT void CALL ShowCFB(void)
{
    // gRSPQueue.async(plugin_draw);
}

/******************************************************************
  Function: UpdateScreen
  Purpose:  This function is called in response to a vsync of the
            screen were the VI bit in MI_INTR_REG has already been
            set
  input:    none
  output:   none
*******************************************************************/
EXPORT void CALL UpdateScreen(void)
{
    gRSPQueue.async(plugin_draw);
    // ShowCFB();
}

/******************************************************************
  Function: ViStatusChanged
  Purpose:  This function is called to notify the dll that the
            ViStatus registers value has been changed.
  input:    none
  output:   none
*******************************************************************/
EXPORT void CALL ViStatusChanged(void)
{

}

/******************************************************************
  Function: ViWidthChanged
  Purpose:  This function is called to notify the dll that the
            ViWidth registers value has been changed.
  input:    none
  output:   none
*******************************************************************/
EXPORT void CALL ViWidthChanged(void)
{

}