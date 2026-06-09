#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <shellapi.h> // For CommandLineToArgvW

// The min/max macros conflict with like-named member functions.
// Only use std::min and std::max defined in <algorithm>.

#if defined(min)
#undef min
#endif

#if defined(max)
#undef max
#endif

// In order to define a function called CreateWindow, the Windows macro needs to
// be undefined.
#if defined(CreateWindow)
#undef CreateWindow
#endif

// Windows Runtime Library. Needed for Microsoft::WRL::ComPtr<> template class.

#include <wrl.h>

using namespace Microsoft::WRL;

// DirectX 12 specific headers.
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>

// D3D12 extension library.
#include <d3dx12.h>

// STL Headers
#include <algorithm>
#include <cassert>
#include <chrono>

// Helper functions
#include <Helpers.h>

// the number of swapchain back buffers
const uint8_t g_NumFrames = 3;

// Use WARP adapter
bool g_UseWarp = false;

uint32_t g_ClientWidth = 1280;
uint32_t g_ClientHeight = 720;

// Set to true once the DX12 objects have been initialized.
bool g_IsInitialized = false;

// Window handle that will be used to display the rendered image
HWND g_handleWindow;

// Window rectangle (used to toggle fullscreen state) used to store previous window dimensions before going to fullscreen mode.
// the previous size of window needs to be stored so that when switching back to windowed mode, the window dimensions can be restored correctly.
RECT g_WindowRectangle;

// DirectX 12 Objects
ComPtr<ID3D12Device2> g_Device;                                         // DirectX 12 device object 
ComPtr<ID3D12CommandQueue> g_CommandQueue;                              // Command Queue            
ComPtr<IDXGISwapChain4> g_SwapChain;                                    // Swap Chain, responsible for presenting rendered image to the window
ComPtr<ID3D12Resource> g_BackBuffers[g_NumFrames];                      // in order to transition the back buffer resources to correct state, pointers to back buffer resources are tracked in this array
ComPtr<ID3D12GraphicsCommandList> g_CommandList;                        // generally a single command list is needed to record GPU commands using a single thread. This example uses main thread to record all GPU commands, so only one command list is defined.
ComPtr<ID3D12CommandAllocator> g_CommandAllocators[g_NumFrames];        // backing memory for recording GPU commands into a command list.
ComPtr<ID3D12DescriptorHeap> g_RTVDescriptorHeap;                       // array of render target views (RTVs)
UINT g_RTVDescriptorSize;                                               // size of a single RTV descriptor
UINT g_CurrentBackBufferIndex;                                          // used to store the index of the current back buffer of the swapchain

// Synchronization objects
ComPtr<ID3D12Fence> g_Fence;                                            // Fence object used for GPU-CPU synchronization
uint64_t g_FenceValue = 0;                                              // next fence value to signal the command queue with
uint64_t g_FrameFenceValues[g_NumFrames] = {};                          // used to keep track of the fence values that were used to signal command queue for a particular frame
HANDLE g_FenceEvent;                                                    // handle to OS event object used to receive notification that the fence has reach a specific value

// Swapchain Present Method
// by default, enable V-sync. Toggle with V key
bool g_VSync = true;                                                    // whether swap chain present method should wait for the next vertical refresh before presenting the rendered img.
bool g_TearingSupported = false;

// by default, use windowed mode. Toggle with F11
bool g_Fullscreen = false;

// Window callback function
LRESULT CALLBACK WindowProcess(HWND, UINT, WPARAM, LPARAM);

// allows a few globally defined variables to be overriden with cmd line arguments
void ParseCommandLineArguments()
{
    int argc;
    wchar_t** argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);

    for (size_t i = 0; i < argc; ++i)
    {
        if (::wcscmp(argv[i], L"-w") == 0 || ::wcscmp(argv[i], L"--width") == 0)
        {
            g_ClientWidth = ::wcstol(argv[++i], nullptr, 10);
        }
        if (::wcscmp(argv[i], L"-h") == 0 || ::wcscmp(argv[i], L"--height") == 0)
        {
            g_ClientHeight = ::wcstol(argv[++i], nullptr, 10);
        }
        if (::wcscmp(argv[i], L"-warp") == 0 || ::wcscmp(argv[i], L"--warp") == 0)
        {
            g_UseWarp = true;
        }
        if (::wcscmp(argv[i], L"-f") == 0 || ::wcscmp(argv[i], L"--fullscreen") == 0)
        {
            g_Fullscreen = true;
        }
    }

    // Free memory allocated by CommandLineToArgvW
    // :: identifies system functions that are defined in global scope.
    ::LocalFree(argv);
    // functions defined in scope of source file do not use this notation
}

void EnableDebugLayer()
{
#if defined(_DEBUG)
    // Always enable the debug layer before doing anything DX12 related
    // so all possible errors generated while creating DX12 objects
    // are caught by the debug layer.
    ComPtr<ID3D12Debug> debugInterface;
    ThrowIfFailed(D3D12GetDebugInterface(IID_PPV_ARGS(&debugInterface)));
    debugInterface->EnableDebugLayer();
#endif
}

void RegisterWindowClass(HINSTANCE handleInstance, const wchar_t* windowClassName )
{
    // Register a window class for creating our render window with.
    WNDCLASSEXW windowClass = {};

    windowClass.cbSize = sizeof(WNDCLASSEX);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = &WindowProcess;
    windowClass.cbClsExtra = 0;
    windowClass.cbWndExtra = 0;
    windowClass.hInstance = handleInstance;
    windowClass.hIcon = ::LoadIcon(handleInstance, NULL);
    windowClass.hCursor = ::LoadCursor(NULL, IDC_ARROW);
    windowClass.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    windowClass.lpszMenuName = NULL;
    windowClass.lpszClassName = windowClassName;
    windowClass.hIconSm = ::LoadIcon(handleInstance, NULL);

    static ATOM atom = ::RegisterClassExW(&windowClass);
    assert(atom > 0);
}

// Process Window
HWND CreateWindow(
    const wchar_t* windowClassName, 
    HINSTANCE handleInstance,
    const wchar_t* windowTitle, 
    uint32_t width, 
    uint32_t height)
{
    // retrieves specific system metric info
    int screenWidth = ::GetSystemMetrics(SM_CXSCREEN);
    int screenHeight = ::GetSystemMetrics(SM_CYSCREEN);

    RECT windowRect = { 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };
    ::AdjustWindowRect(&windowRect, WS_OVERLAPPEDWINDOW, FALSE);

    int windowWidth = windowRect.right - windowRect.left;
    int windowHeight = windowRect.bottom - windowRect.top;

    // Center the window within the screen. Clamp to 0, 0 for the top-left corner.
    int windowX = std::max<int>(0, (screenWidth - windowWidth) / 2);
    int windowY = std::max<int>(0, (screenHeight - windowHeight) / 2);

    HWND handleWindow = ::CreateWindowExW(
        NULL,
        windowClassName,
        windowTitle,
        WS_OVERLAPPEDWINDOW,
        windowX,
        windowY,
        windowWidth,
        windowHeight,
        NULL,
        NULL,
        handleInstance,
        nullptr
    );

    assert(handleWindow && "Failed to create window");

    return handleWindow;
}

// Query DirectX 12 Adapter
ComPtr<IDXGIAdapter4> GetAdapter(bool useWarp)
{
    ComPtr<IDXGIFactory4> dxgiFactory;
    UINT createFactoryFlags = 0;

#if defined(_DEBUG)
    // this flag should not be used in production builds
    createFactoryFlags = DXGI_CREATE_FACTORY_DEBUG;
#endif
    
    ThrowIfFailed(CreateDXGIFactory2(createFactoryFlags, IID_PPV_ARGS(&dxgiFactory)));

    // there's 2 interfaces because 1 is the old DXGI 1.1 interface.
    ComPtr<IDXGIAdapter1> dxgiAdapter1;
    // 4 is the modern one but since EnumWarpAdapter only supports up to 1, we use 1 then upcast.
    ComPtr<IDXGIAdapter4> dxgiAdapter4;

    if (useWarp)
    {
        ThrowIfFailed(dxgiFactory->EnumWarpAdapter(IID_PPV_ARGS(&dxgiAdapter1)));
        ThrowIfFailed(dxgiAdapter1.As(&dxgiAdapter4));
    }
    else
    {
        SIZE_T maxDedicatedVideoMemory = 0;
        // enumerate the available GPU adapters ont he system
        for (UINT i = 0; dxgiFactory->EnumAdapters1(i, &dxgiAdapter1) != DXGI_ERROR_NOT_FOUND; ++i)
        {
            DXGI_ADAPTER_DESC1 dxgiAdapterDesc1;
            dxgiAdapter1->GetDesc1(&dxgiAdapterDesc1);

            if (
                // only get hardware adapters
                (dxgiAdapterDesc1.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0 
                &&
                // check to see if the adapter can create a D3D12 device without actually creating it.
                SUCCEEDED(
                    D3D12CreateDevice(
                        dxgiAdapter1.Get(), 
                        D3D_FEATURE_LEVEL_11_0,
                        __uuidof(ID3D12Device), 
                        nullptr)) 
                &&
                // The adapter with the largest dedicated video memory is favored
                dxgiAdapterDesc1.DedicatedVideoMemory > maxDedicatedVideoMemory )
            {
                maxDedicatedVideoMemory = dxgiAdapterDesc1.DedicatedVideoMemory;
                ThrowIfFailed(dxgiAdapter1.As(&dxgiAdapter4));
            }
        }
    }

    return dxgiAdapter4;
}

// Create the DirectX 12 device
ComPtr<ID3D12Device2> CreateDevice(ComPtr<IDXGIAdapter4> adapter)
{
    ComPtr<ID3D12Device2> d3d12Device2;

    ThrowIfFailed(D3D12CreateDevice(
        adapter.Get(),
        D3D_FEATURE_LEVEL_11_0, // minimum features level required for successful device creation
        IID_PPV_ARGS(&d3d12Device2)));

    // enable debug messages in debug mode
#if defined(_DEBUG)
    ComPtr<ID3D12InfoQueue> pointerInfoQueue;
    if (SUCCEEDED(d3d12Device2.As(&pointerInfoQueue)))
    {
        pointerInfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, TRUE);
        pointerInfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, TRUE);
        pointerInfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, TRUE);

        // Suppress whole categories of messages
        //D3D12_MESSAGE_CATEGORY Categories[] = {};

        // Suppress messages based on their severity level
        D3D12_MESSAGE_SEVERITY Severities[] =
        {
            D3D12_MESSAGE_SEVERITY_INFO
        };

        // Suppress individual messages by their ID
        D3D12_MESSAGE_ID DenyIds[] = {
            D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE,   // I'm really not sure how to avoid this message.
            D3D12_MESSAGE_ID_MAP_INVALID_NULLRANGE,                         // This warning occurs when using capture frame while graphics debugging.
            D3D12_MESSAGE_ID_UNMAP_INVALID_NULLRANGE,                       // This warning occurs when using capture frame while graphics debugging.
        };

        D3D12_INFO_QUEUE_FILTER NewFilter = {};
        //NewFilter.DenyList.NumCategories = _countof(Categories);
        //NewFilter.DenyList.pCategoryList = Categories;
        NewFilter.DenyList.NumSeverities = _countof(Severities);
        NewFilter.DenyList.pSeverityList = Severities;
        NewFilter.DenyList.NumIDs = _countof(DenyIds);
        NewFilter.DenyList.pIDList = DenyIds;

        ThrowIfFailed(pInfoQueue->PushStorageFilter(&NewFilter));
    }
#endif

    return d3d12Device2;
}

// Create Command Queue
ComPtr<ID3D12CommandQueue> CreateCommandQueue(ComPtr<ID3D12Device2> device, D3D12_COMMAND_LIST_TYPE type)
{
    ComPtr<ID3D12CommandQueue> d3d12CommandQueue;

    D3D12_COMMAND_QUEUE_DESC cmdQueueDescriptor = {};
    cmdQueueDescriptor.Type =     type;                                     // can be direct, compute, or copy
    cmdQueueDescriptor.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;      // normal, high, global realtime
    cmdQueueDescriptor.Flags =    D3D12_COMMAND_QUEUE_FLAG_NONE;            // specified additional flags from D3D12_COMMAND_QUEUE_FLAGS
    cmdQueueDescriptor.NodeMask = 0;                                        // for single GPU operation, 0. for multiple GPU nodes, set bit to identify the node which the cmd queue applies

    ThrowIfFailed(device->CreateCommandQueue(&cmdQueueDescriptor, IID_PPV_ARGS(&d3d12CommandQueue)));

    return d3d12CommandQueue;
}

// Check for Tearing Support
bool CheckTearingSupport()
{
    BOOL allowTearing = FALSE;

    // Rather than create the DXGI 1.5 factory interface directly, we create the
    // DXGI 1.4 interface and query for the 1.5 interface. This is to enable the 
    // graphics debugging tools which will not support the 1.5 factory interface 
    // until a future update.
    ComPtr<IDXGIFactory4> factory4;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory4))))
    {
        ComPtr<IDXGIFactory5> factory5;
        if (SUCCEEDED(factory4.As(&factory5)))
        {
            if (FAILED(factory5->CheckFeatureSupport(
                DXGI_FEATURE_PRESENT_ALLOW_TEARING, 
                &allowTearing, sizeof(allowTearing))))
            {
                allowTearing = FALSE;
            }
        }
    }

    return allowTearing == TRUE;
}

// Create Swap Chain
ComPtr<IDXGISwapChain4> CreateSwapChain(
    HWND handleWindow, 
    ComPtr<ID3D12CommandQueue> commandQueue, 
    uint32_t width,
    uint32_t height, 
    uint32_t bufferCount)
{
    ComPtr<IDXGISwapChain4> dxgiSwapChain4;
    ComPtr<IDXGIFactory4> dxgiFactory4;
    UINT createFactoryFlags = 0;
#if defined(_DEBUG)
    createFactoryFlags = DXGI_CREATE_FACTORY_DEBUG;
#endif

    ThrowIfFailed(CreateDXGIFactory2(createFactoryFlags, IID_PPV_ARGS(&dxgiFactory4)));

    DXGI_SWAP_CHAIN_DESC1 swapChainDescriptor = {};
    swapChainDescriptor.Width = width;
    swapChainDescriptor.Height = height;
    swapChainDescriptor.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDescriptor.Stereo = FALSE; // If you specify stereo, you must also specify a flip-model swap chain.
    swapChainDescriptor.SampleDesc = { 1, 0 }; // multisampling parameters, valid only with bit-block transfer. when using flip, this must be {1, 0}
    swapChainDescriptor.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; // back buffer can be used for shader input or RT output
    swapChainDescriptor.BufferCount = bufferCount; // the minimum # of buffers for flip presentation model is 2 (front and back)
    swapChainDescriptor.Scaling = DXGI_SCALING_STRETCH; // resize behavior if size of backbufer is not equal to target output
    swapChainDescriptor.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD; // presentation model used by swapchain [sequential | discard]
    swapChainDescriptor.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED; // transparency behavior of swapchain
    swapChainDescriptor.Flags = CheckTearingSupport() ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0; // flag should always be specified if tear support is available

    ComPtr<IDXGISwapChain1> swapChain1;
    ThrowIfFailed(dxgiFactory4->CreateSwapChainForHwnd(
        commandQueue.Get(), // pointer to direct cmd queue, this param cannot be null
        handleWindow, // handle associated with swap chain, cannot be null
        &swapChainDescriptor, // pointer to swapchain descriptor, cannot be null
        nullptr, // pointer to full screen description, can be null when using windowed swap chain
        nullptr, // pointer to dxgi output interface to restrict content to, can be null for no restrictions to output target
        &swapChain1)); // output of this function call, double ptr to swapchain interface that this function creates

    // Disable the Alt+Enter fullscreen toggle feature. Switching to fullscreen will be handled manually.
    ThrowIfFailed(dxgiFactory4->MakeWindowAssociation(handleWindow, DXGI_MWA_NO_ALT_ENTER));

    ThrowIfFailed(swapChain1.As(&dxgiSwapChain4));

    return dxgiSwapChain4;
}

// Create a Descriptor Heap
ComPtr<ID3D12DescriptorHeap> CreateDescriptorHeap(
    ComPtr<ID3D12Device2> device,
    D3D12_DESCRIPTOR_HEAP_TYPE type, // constant-buffer (CB), shader-resource (SRV), unordered-access (UAV), render-target (RTV), depth-stencil (DSV) or sampler
    uint32_t numDescriptors) // number of descriptors in the heap
{
    ComPtr<ID3D12DescriptorHeap> descriptorHeap;

    D3D12_DESCRIPTOR_HEAP_DESC desc = {};
    desc.NumDescriptors = numDescriptors;
    desc.Type = type;

    ThrowIfFailed(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&descriptorHeap)));

    return descriptorHeap;
}

// Create Render Target Views (RTVs)
void UpdateRenderTargetViews(
    ComPtr<ID3D12Device2> device,
    ComPtr<IDXGISwapChain4> swapChain, 
    ComPtr<ID3D12DescriptorHeap> descriptorHeap)
{
    // size of single descriptor is vendor specific, query it 
    auto rtvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(descriptorHeap->GetCPUDescriptorHandleForHeapStart());

    for (int i = 0; i < g_NumFrames; ++i)
    {
        ComPtr<ID3D12Resource> backBuffer;
        ThrowIfFailed(swapChain->GetBuffer(i, IID_PPV_ARGS(&backBuffer)));

        device->CreateRenderTargetView(backBuffer.Get(), nullptr, rtvHandle);

        g_BackBuffers[i] = backBuffer;

        rtvHandle.Offset(rtvDescriptorSize);
    }
}