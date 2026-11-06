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
    if(!inspect && !confirm && !capture){std::fputs("Use --inspect-game-focus, --menu-key confirm, or --capture-window <png>\n",stderr);return 2;}
    SetProcessDPIAware();Client c;EnumWindows(FindClient,reinterpret_cast<LPARAM>(&c));
    if(!c.window || c.ambiguous){std::fputs("No unique visible Far Cry 2 client found\n",stderr);return 1;}
    if(inspect){std::printf("{\"pid\":%lu,\"foreground\":%d}\n",static_cast<unsigned long>(c.pid),GetAncestor(GetForegroundWindow(),GA_ROOT)==GetAncestor(c.window,GA_ROOT));return 0;}
    if(capture)return Capture(c.window,argv[2])?0:1;
    if(!Focus(c.window)){std::fputs("Game focus failed; no key sent\n",stderr);return 1;}
    INPUT key{};key.type=INPUT_KEYBOARD;key.ki.wScan=0x1c;key.ki.dwFlags=KEYEVENTF_SCANCODE;
    const bool down=SendInput(1,&key,sizeof key)==1;Sleep(100);key.ki.dwFlags|=KEYEVENTF_KEYUP;
    const bool up=SendInput(1,&key,sizeof key)==1;
    return down && up?0:1;
}
