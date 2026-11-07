// Focused Windows helper for the optional native VR regression tests.
// It only targets visible Far Cry 2 clients. No controller, save or fire input.
#define NOMINMAX
#include <windows.h>
#include <wincodec.h>
#include <cstdio>
#include <string>
#include <vector>

struct Client { HWND window=nullptr;DWORD pid=0;bool ambiguous=false; };
static BOOL CALLBACK FindClient(HWND w,LPARAM value) {
    if(!IsWindowVisible(w))return TRUE;
    RECT r{};if(!GetClientRect(w,&r) || r.right<2 || r.bottom<2)return TRUE;
    DWORD pid=0;GetWindowThreadProcessId(w,&pid);
    HANDLE p=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);
    if(!p)return TRUE;
    wchar_t path[32768]{};DWORD size=32768;
    const bool found=QueryFullProcessImageNameW(p,0,path,&size)!=FALSE;CloseHandle(p);
    if(!found)return TRUE;
    const wchar_t* name=wcsrchr(path,L'\\');name=name?name+1:path;
    if(_wcsicmp(name,L"FarCry2.exe") && _wcsicmp(name,L"FarCry2VR.exe"))return TRUE;
    wchar_t cls[64]{};GetClassNameW(w,cls,64);
    if(wcscmp(cls,L"Nomad"))return TRUE;
    auto& c=*reinterpret_cast<Client*>(value);
    if(c.window && c.pid!=pid)c.ambiguous=true;
    c.window=w;c.pid=pid;return TRUE;
}
static bool Focus(HWND w) {
    const DWORD current=GetCurrentThreadId(),foreground=GetWindowThreadProcessId(GetForegroundWindow(),nullptr);
    const bool attached=foreground && current!=foreground && AttachThreadInput(current,foreground,TRUE);
    ShowWindow(w,SW_RESTORE);BringWindowToTop(w);SetForegroundWindow(w);
    if(attached)AttachThreadInput(current,foreground,FALSE);
    Sleep(100);
    return GetAncestor(GetForegroundWindow(),GA_ROOT)==GetAncestor(w,GA_ROOT);
}
// Bounded native console setup for an unsaved owned audit session. Commands
// are restricted to the game's weapon grants and daylight for pose captures.
static bool AuditCommand(const wchar_t* text){
    if(!wcscmp(text,L"env_hour 12"))return true;
    constexpr const wchar_t* weapons[]={L"Machete",L"Flare_Gun",L"PKM",L"M249_Saw",L"Mortar",L"Dart_Rifle",L"M1903",L"Carl_Gustaf",L"RPG7",L"LPO50",L"FNFAL",L"AK47",L"G3KA4",L"MP5",L"AS50",L"SPAS12",L"MGL140",L"Ithaca",L"USAS12",L"M16",L"Dragunov",L"SilencedMakarov_6P9",L"DesertEagle",L"Makarov",L"MAC10",L"Star45",L"M79",L"Uzi",L"IED"};
    for(const auto* weapon:weapons)if(std::wstring(L"domino_")+weapon==text)return true;
    return false;
}
static bool Tap(HWND w,WORD scan){
    if(GetAncestor(GetForegroundWindow(),GA_ROOT)!=GetAncestor(w,GA_ROOT))return false;
    INPUT key{};key.type=INPUT_KEYBOARD;key.ki.wScan=scan;key.ki.dwFlags=KEYEVENTF_SCANCODE;
    const bool down=SendInput(1,&key,sizeof key)==1;Sleep(150);key.ki.dwFlags|=KEYEVENTF_KEYUP;
    return SendInput(1,&key,sizeof key)==1 && down;
}
static bool Capture(HWND w,const wchar_t* path);
static bool Console(HWND w,const wchar_t* command,const wchar_t* diagnostic=nullptr){
    if(!Tap(w,0x29))return false;Sleep(800);
    if(diagnostic)Capture(w,(std::wstring(diagnostic)+L"-open.png").c_str());
    const HKL layout=GetKeyboardLayout(GetWindowThreadProcessId(w,nullptr));
    for(const auto* ch=command;*ch;++ch){
        if(GetAncestor(GetForegroundWindow(),GA_ROOT)!=GetAncestor(w,GA_ROOT))return false;
        const SHORT mapped=VkKeyScanExW(*ch,layout);
        if(mapped==-1 || (mapped&0xfe00))return false;
        const bool shift=(mapped&0x100)!=0;
        INPUT modifier{};modifier.type=INPUT_KEYBOARD;modifier.ki.wScan=0x2a;modifier.ki.dwFlags=KEYEVENTF_SCANCODE;
        if(shift && SendInput(1,&modifier,sizeof modifier)!=1)return false;
        const bool typed=Tap(w,WORD(MapVirtualKeyExW(mapped&255,MAPVK_VK_TO_VSC,layout)));
        modifier.ki.dwFlags|=KEYEVENTF_KEYUP;
        const bool released=!shift || SendInput(1,&modifier,sizeof modifier)==1;
        if(!typed || !released)return false;Sleep(75);
    }
    Sleep(200);if(diagnostic)Capture(w,(std::wstring(diagnostic)+L"-text.png").c_str());
    if(!Tap(w,0x1c))return false;Sleep(1200);
    if(diagnostic)Capture(w,(std::wstring(diagnostic)+L"-result.png").c_str());
    return Tap(w,0x29);
}
template<class T> static void Release(T*& value){if(value){value->Release();value=nullptr;}}
static bool Capture(HWND w,const wchar_t* path) {
    RECT r{};if(!GetClientRect(w,&r) || r.right<2 || r.bottom<2 || r.right>16384 || r.bottom>16384)return false;
    const UINT width=static_cast<UINT>(r.right),height=static_cast<UINT>(r.bottom);
    HDC screen=GetDC(w),memory=screen?CreateCompatibleDC(screen):nullptr;
    BITMAPINFO bi{};bi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);bi.bmiHeader.biWidth=r.right;bi.bmiHeader.biHeight=-r.bottom;
    bi.bmiHeader.biPlanes=1;bi.bmiHeader.biBitCount=32;bi.bmiHeader.biCompression=BI_RGB;
    void* bits=nullptr;HBITMAP bitmap=memory?CreateDIBSection(screen,&bi,DIB_RGB_COLORS,&bits,nullptr,0):nullptr;
    HGDIOBJ previous=bitmap?SelectObject(memory,bitmap):nullptr;
    bool ok=bitmap && BitBlt(memory,0,0,r.right,r.bottom,screen,0,0,SRCCOPY|CAPTUREBLT);
    std::vector<BYTE> pixels;
    if(ok){const auto* data=static_cast<const BYTE*>(bits);pixels.assign(data,data+static_cast<size_t>(width)*height*4);for(size_t i=3;i<pixels.size();i+=4)pixels[i]=255;}
    if(previous)SelectObject(memory,previous);
    if(bitmap)DeleteObject(bitmap);if(memory)DeleteDC(memory);if(screen)ReleaseDC(w,screen);
    if(!ok)return false;
    if(FAILED(CoInitializeEx(nullptr,COINIT_MULTITHREADED)))return false;
    IWICImagingFactory* factory=nullptr;IWICStream* stream=nullptr;IWICBitmapEncoder* encoder=nullptr;IWICBitmapFrameEncode* frame=nullptr;
    WICPixelFormatGUID format=GUID_WICPixelFormat32bppBGRA;
    HRESULT result=CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory));
    if(SUCCEEDED(result))result=factory->CreateStream(&stream);
    if(SUCCEEDED(result))result=stream->InitializeFromFilename(path,GENERIC_WRITE);
    if(SUCCEEDED(result))result=factory->CreateEncoder(GUID_ContainerFormatPng,nullptr,&encoder);
    if(SUCCEEDED(result))result=encoder->Initialize(stream,WICBitmapEncoderNoCache);
    if(SUCCEEDED(result))result=encoder->CreateNewFrame(&frame,nullptr);
    if(SUCCEEDED(result))result=frame->Initialize(nullptr);
    if(SUCCEEDED(result))result=frame->SetSize(width,height);
    if(SUCCEEDED(result))result=frame->SetPixelFormat(&format);
    if(SUCCEEDED(result) && !IsEqualGUID(format,GUID_WICPixelFormat32bppBGRA))result=E_FAIL;
    if(SUCCEEDED(result))result=frame->WritePixels(height,width*4,static_cast<UINT>(pixels.size()),pixels.data());
    if(SUCCEEDED(result))result=frame->Commit();
    if(SUCCEEDED(result))result=encoder->Commit();
    Release(frame);Release(encoder);Release(stream);Release(factory);CoUninitialize();
    return SUCCEEDED(result);
}
int wmain(int argc,wchar_t** argv) {
    const bool inspect=argc==2 && !wcscmp(argv[1],L"--inspect-game-focus");
    const bool confirm=argc==3 && !wcscmp(argv[1],L"--menu-key") && !wcscmp(argv[2],L"confirm");
    const bool capture=argc==3 && !wcscmp(argv[1],L"--capture-window");
    const bool handToggle=argc==2 && !wcscmp(argv[1],L"--weapon-hand-toggle");
    const bool weaponSlot=argc==3 && !wcscmp(argv[1],L"--weapon-slot") && wcslen(argv[2])==1 && argv[2][0]>=L'1' && argv[2][0]<=L'4';
    const bool console=(argc==3 || argc==4) && !wcscmp(argv[1],L"--audit-console") && AuditCommand(argv[2]);
    if(!inspect && !confirm && !capture && !handToggle && !weaponSlot && !console){std::fputs("Use --inspect-game-focus, --menu-key confirm, --weapon-hand-toggle, --weapon-slot 1..4, --audit-console <allowed command>, or --capture-window <png>\n",stderr);return 2;}
    SetProcessDPIAware();Client c;EnumWindows(FindClient,reinterpret_cast<LPARAM>(&c));
    if(!c.window || c.ambiguous){std::fputs("No unique visible Far Cry 2 client found\n",stderr);return 1;}
    if(inspect){std::printf("{\"pid\":%lu,\"foreground\":%d}\n",static_cast<unsigned long>(c.pid),GetAncestor(GetForegroundWindow(),GA_ROOT)==GetAncestor(c.window,GA_ROOT));return 0;}
    if(capture)return Capture(c.window,argv[2])?0:1;
    if(!Focus(c.window)){std::fputs("Game focus failed; no key sent\n",stderr);return 1;}
    if(console)return Console(c.window,argv[2],argc==4?argv[3]:nullptr)?0:1;
    INPUT key{};key.type=INPUT_KEYBOARD;key.ki.wScan=weaponSlot?WORD(2+argv[2][0]-L'1'):handToggle?0x3b:0x1c;key.ki.dwFlags=KEYEVENTF_SCANCODE;
    const bool down=SendInput(1,&key,sizeof key)==1;Sleep(handToggle?350:100);key.ki.dwFlags|=KEYEVENTF_KEYUP;
    const bool up=SendInput(1,&key,sizeof key)==1;
    return down && up?0:1;
}
