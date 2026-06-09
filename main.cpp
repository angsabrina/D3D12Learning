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
#include "d3dx12.h"

// STL Headers
#include <algorithm>
#include <cassert>
#include <chrono>

// Helper functions
#include "Helpers.h"

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
        D3D_FEATURE_LEVEL_11_0,             // minimum features level required for successful device creation
        IID_PPV_ARGS(&d3d12Device2)));      // [out] pointer to access device

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

        ThrowIfFailed(pointerInfoQueue->PushStorageFilter(&NewFilter));
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

    ThrowIfFailed(device->CreateCommandQueue(
        &cmdQueueDescriptor, 
        IID_PPV_ARGS(&d3d12CommandQueue)));

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

    // handle to first descriptor in the heap
    CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(descriptorHeap->GetCPUDescriptorHandleForHeapStart());

    for (int i = 0; i < g_NumFrames; ++i)
    {
        ComPtr<ID3D12Resource> backBuffer;
        // points to the i-th buffer in swapchain
        ThrowIfFailed(swapChain->GetBuffer(i, IID_PPV_ARGS(&backBuffer)));

        device->CreateRenderTargetView(
            backBuffer.Get(),   // pointer to resource that contains render target texture
            nullptr,            // null used to create default descriptoir for resource
            rtvHandle);         // handle to descriptor where view is placed

        // pointer to buffer stored so the resource can be transitioned to correct state
        g_BackBuffers[i] = backBuffer;

        // descriptor handle incremented to next handle in descriptor heap
        rtvHandle.Offset(rtvDescriptorSize);
    }
}

// Create Command Allocators
// the backing memory for a command list, can only be used by 1 cmd list at a time but can be reused after the commands on that list have been executed by GPU
// a fence is used to check if the GPU cmd have finished executing.
ComPtr<ID3D12CommandAllocator> CreateCommandAllocator(ComPtr<ID3D12Device2> device, D3D12_COMMAND_LIST_TYPE type)
{
    // this function only creates 1 command allocator, but later we use this function to create multiple cmd allocators.
    ComPtr<ID3D12CommandAllocator> commandAllocator;
    // types of cmd allocators: [direct (doesn't inherit GPU state), bundle (inherits all GPU state), compute, copy]
    ThrowIfFailed(device->CreateCommandAllocator(type, IID_PPV_ARGS(&commandAllocator)));

    return commandAllocator;
}

// Create Command List
// for D3D12 execution of cmd recorded into a cmd list are ALWAYS deferred. ie invoking draw/dispatch cmds are not executed until the cmd list is sent to cmd queue
// cmd list can be reused immediately after it has been executed on cmd queue
ComPtr<ID3D12GraphicsCommandList> CreateCommandList(
    ComPtr<ID3D12Device2> device,
    ComPtr<ID3D12CommandAllocator> commandAllocator, 
    D3D12_COMMAND_LIST_TYPE type)
{
    ComPtr<ID3D12GraphicsCommandList> commandList;
    ThrowIfFailed(device->CreateCommandList(
        0,                              // for single GPU operation 0, for multiple GPU nodes set bit to identify node
        type,                           // type: [direct, bundle, compute, copy]
        commandAllocator.Get(),         // pointer to cmd allocator that devices creates cmd list from
        nullptr,                        // pointer to pipeline state object with initial pipeline state for cmd list. NULL sets dummy PS so that drivers don't have undefined state
        IID_PPV_ARGS(&commandList)));   // [out] pointer access cmd list
    
    // the list is closed here so that it can be reset before reused for recording cmds
    ThrowIfFailed(commandList->Close());

    return commandList;
}

// Create (a) Fence
// synchronization object for either CPU/GPU
// rule of thumb: fence initialized with 0 and value only increases. fence is reached when it is equal to or greater than specific fence value
ComPtr<ID3D12Fence> CreateFence(ComPtr<ID3D12Device2> device)
{
    ComPtr<ID3D12Fence> fence;

    ThrowIfFailed(device->CreateFence(
        0,                          // initial fence value
        D3D12_FENCE_FLAG_NONE,      // flags [none, shared, shared cross adapter, non monitored]
        IID_PPV_ARGS(&fence)));     // [out] pointer to access fence

    return fence;
}

// Create Event
// if fence has not been signaled with specific value, CPU thread needs to use an event handle to block further processing until fence has been signaled
HANDLE CreateEventHandle()
{
    HANDLE fenceEvent;
    
    fenceEvent = ::CreateEvent(
        NULL,   // pointer to security attributes, NULL means handle cannot be inherited by child processes
        FALSE,  // If TRUE creates a manual reset object, if FALSE auto-reset event object system automatically resets after the waiting thread has been release
        FALSE,  // if TRUE initial state of event object is signaled, otherise non-signaled
        NULL);  // name of the event, if NULL event object is created without a name.
    assert(fenceEvent && "Failed to create fence event.");

    return fenceEvent;
}

// Signal the Fence
// when using the D3D12CommandQueue::Signal method, fence is not immediately signaled but only when the cmmd queue has reach that point during execution.
// Any commands queued before signal was invoked must complete execution before fence will be signaled
uint64_t Signal(
    ComPtr<ID3D12CommandQueue> commandQueue, 
    ComPtr<ID3D12Fence> fence,
    uint64_t& fenceValue)
{
    // pre-increment because the Signal needs a higher fence value than whatever it previous had
    uint64_t fenceValueForSignal = ++fenceValue;
    ThrowIfFailed(commandQueue->Signal(fence.Get(), fenceValueForSignal));

    return fenceValueForSignal;
}

// used to stall CPU thread if fence has not reached a specific value.
void WaitForFenceValue(
    ComPtr<ID3D12Fence> fence,
    uint64_t fenceValue, 
    HANDLE fenceEvent,
    std::chrono::milliseconds duration = std::chrono::milliseconds::max()) // this duration default is about 300 million years
{
    // currently completed fence value gotten with GetCompletedValue()
    if (fence->GetCompletedValue() < fenceValue)
    {
        ThrowIfFailed(fence->SetEventOnCompletion(fenceValue, fenceEvent));
        ::WaitForSingleObject(fenceEvent, static_cast<DWORD>(duration.count()));
    }
}

// Flush the GPU
// used to ensure GPU has finished processing all commands before continuing program on CPU
// strongly advised to flush GPU cmd queue before releasing any resources that might be referenced by a cmd list currently 'in flight' on the cmd queue
void Flush(
    ComPtr<ID3D12CommandQueue> commandQueue,
    ComPtr<ID3D12Fence> fence,
    uint64_t& fenceValue,
    HANDLE fenceEvent)
{
    // add a Signal to to the end of the command queue so when this function returns it guarantees a completely empty queue
    uint64_t newFenceValueSignal = Signal(commandQueue, fence, fenceValue);
    // this function will block the calling thread (CPU) until the fence value has been reached. 
    //after this returns it is safe to release any resources that were refereced by the GPU
    WaitForFenceValue(fence, newFenceValueSignal, fenceEvent);
}

// Update
// purpose: display the frame-rate each second in debug output in VS
void Update()
{
    static uint64_t frameCounter = 0;   // number of times a frame was rendered to screen since last frame-rate was printed
    static double elapsedSeconds = 0.0; // time since last framerate was printed
    static std::chrono::high_resolution_clock clock;
    static auto t0 = clock.now();

    // each frame, framecounter incremented and delta time is computed
    frameCounter++;
    auto t1 = clock.now();
    auto deltaTime = t1 - t0;
    t0 = t1;

    elapsedSeconds += deltaTime.count() * 1e-9;
    if (elapsedSeconds > 1.0)
    {
        char buffer[500];
        auto fps = frameCounter / elapsedSeconds;
        sprintf_s(buffer, 500, "FPS: %f\n", fps);
        OutputDebugStringA(buffer);

        frameCounter = 0;
        elapsedSeconds = 0.0;
    }
}

// Render
// for this app, 2 main parts: clear back buffer/render target and present rendered frame
void Render() 
{   
    //pointers to command allocator and back buffer resource are retrieved according to current backbuffer index 
    auto commandAllocator = g_CommandAllocators[g_CurrentBackBufferIndex];
    auto backBuffer = g_BackBuffers[g_CurrentBackBufferIndex];

    // reset command allocator and command list to initial state
    commandAllocator->Reset();
    g_CommandList->Reset(commandAllocator.Get(), nullptr);

    // before the render target can be cleared, it must be transitioned to the RENDER_TARGET state
    {
        // helper struct that allows for easy initializing of various resource barriers
        CD3DX12_RESOURCE_BARRIER resourceBarrier = CD3DX12_RESOURCE_BARRIER::Transition(
            backBuffer.Get(),
            D3D12_RESOURCE_STATE_PRESENT,           // must specify both before and after state of resource
            D3D12_RESOURCE_STATE_RENDER_TARGET);    // state of resource cannot be queried from the resource itself
        g_CommandList-> ResourceBarrier(1, &resourceBarrier);

        // now render target can be cleared.
        float clearColor[] = { 0.4f, 0.6f, 0.9f, 1.0f };
        
        // a CPU descriptor handle to a RTV is stored in the rtvHandle variable
        CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(
            g_RTVDescriptorHeap->GetCPUDescriptorHandleForHeapStart(),
            g_CurrentBackBufferIndex,
            g_RTVDescriptorSize
        );

        g_CommandList->ClearRenderTargetView(
            rtvHandle,      // specifies a render target to be cleared
            clearColor,     // 4-component array that represent the color to fill the render target with
            0,              // the number of rectables in the array that pRects param specifies
            nullptr);       // array of D3D12_RECT structures for rectanges in resource view to clear. If NULL, it clears the entire resource view
    }

    // after clearing, the current back buffer is presented to the screen
    // before presenting the backbuffer resource must be transition to the PRESENT state.
    {
        CD3DX12_RESOURCE_BARRIER resourceBarrier = CD3DX12_RESOURCE_BARRIER::Transition(
            backBuffer.Get(),
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_PRESENT);
        g_CommandList-> ResourceBarrier(1, &resourceBarrier);
    }

    // command list that contains resource transition barrier must be executed on command queue
    // close() method must be called on command list before being executed on the cmd queue 
    ThrowIfFailed(g_CommandList->Close());

    ID3D12CommandList* const commandLists[] = {
        g_CommandList.Get()
    };

    g_CommandQueue->ExecuteCommandLists(_countof(commandLists), commandLists);

    // 
    UINT syncInterval = g_VSync ? 1 : 0;
    UINT presentFlags = g_TearingSupported && !g_VSync ? DXGI_PRESENT_ALLOW_TEARING : 0;
    
    ThrowIfFailed(g_SwapChain->Present(
        syncInterval,       // integer that specifies how to synchronize presentation of a frame with vertical blank, [0-4]
        presentFlags));     // swapchain presentation options. [DXGI_PRESENT_*]

    // immediately after presenting the rendered frame to the screen, a signal is inserted to the queue to stall the GPU thread until resources are finished being used
    g_FrameFenceValues[g_CurrentBackBufferIndex] = Signal(g_CommandQueue, g_Fence, g_FenceValue);

    // after signaling the command queue, index of the current backbuffer is updated
    g_CurrentBackBufferIndex = g_SwapChain->GetCurrentBackBufferIndex();

    // before overwriting the content of the 'new' current backbuffer with content of next frame, CPU thread is stalled
    WaitForFenceValue(g_Fence, g_FrameFenceValues[g_CurrentBackBufferIndex], g_FenceEvent);
}

// Resize
void Resize(uint32_t width, uint32_t height)
{
    if (g_ClientWidth != width || g_ClientHeight != height)
    {
        // Don't allow 0 size swap chain back buffers.
        g_ClientWidth = std::max(1u, width );
        g_ClientHeight = std::max( 1u, height);

        // Flush the GPU queue to make sure the swap chain's back buffers
        // are not being referenced by an in-flight command list.
        Flush(g_CommandQueue, g_Fence, g_FenceValue, g_FenceEvent);

        for (int i = 0; i < g_NumFrames; ++i)
        {
            // Any references to the back buffers must be released
            // before the swap chain can be resized.
            g_BackBuffers[i].Reset();
            g_FrameFenceValues[i] = g_FrameFenceValues[g_CurrentBackBufferIndex];
        }

        DXGI_SWAP_CHAIN_DESC swapChainDesc = {};
        ThrowIfFailed(g_SwapChain->GetDesc(&swapChainDesc));
        ThrowIfFailed(g_SwapChain->ResizeBuffers(g_NumFrames, g_ClientWidth, g_ClientHeight,
            swapChainDesc.BufferDesc.Format, swapChainDesc.Flags));

        g_CurrentBackBufferIndex = g_SwapChain->GetCurrentBackBufferIndex();

        UpdateRenderTargetViews(g_Device, g_SwapChain, g_RTVDescriptorHeap);
    }
}

// Fullscreen
void SetFullscreen(bool fullscreen)
{
    if (g_Fullscreen != fullscreen)
    {
        g_Fullscreen = fullscreen;

        if (g_Fullscreen) // Switching to fullscreen.
        {
            // Store the current window dimensions so they can be restored 
            // when switching out of fullscreen state.
            ::GetWindowRect(g_handleWindow, &g_WindowRectangle);
            
            // Set the window style to a borderless window so the client area fills
            // the entire screen.
            UINT windowStyle = WS_OVERLAPPEDWINDOW & ~(WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX);

            ::SetWindowLongW(g_handleWindow, GWL_STYLE, windowStyle);

            // Query the name of the nearest display device for the window.
            // This is required to set the fullscreen dimensions of the window
            // when using a multi-monitor setup.
            HMONITOR hMonitor = ::MonitorFromWindow(g_handleWindow, MONITOR_DEFAULTTONEAREST);
            MONITORINFOEX monitorInfo = {};
            monitorInfo.cbSize = sizeof(MONITORINFOEX);
            ::GetMonitorInfo(hMonitor, &monitorInfo);

            ::SetWindowPos(g_handleWindow, HWND_TOP,
                monitorInfo.rcMonitor.left,
                monitorInfo.rcMonitor.top,
                monitorInfo.rcMonitor.right - monitorInfo.rcMonitor.left,
                monitorInfo.rcMonitor.bottom - monitorInfo.rcMonitor.top,
                SWP_FRAMECHANGED | SWP_NOACTIVATE);

            ::ShowWindow(g_handleWindow, SW_MAXIMIZE);
        }
        else
        {
            // Restore all the window decorators.
            ::SetWindowLong(g_handleWindow, GWL_STYLE, WS_OVERLAPPEDWINDOW);

            ::SetWindowPos(g_handleWindow, HWND_NOTOPMOST,
                g_WindowRectangle.left,
                g_WindowRectangle.top,
                g_WindowRectangle.right - g_WindowRectangle.left,
                g_WindowRectangle.bottom - g_WindowRectangle.top,
                SWP_FRAMECHANGED | SWP_NOACTIVATE);

            ::ShowWindow(g_handleWindow, SW_NORMAL);
        }
    }
}

// Window Message Procedure
LRESULT CALLBACK WindowProcess(
    HWND hwnd, 
    UINT message, 
    WPARAM wParam, 
    LPARAM lParam)
{
    if ( g_IsInitialized )
    {
        switch (message)
        {
        case WM_PAINT:
            Update();
            Render();
            break;

        // keyboard keys are handled
        case WM_SYSKEYDOWN:
        case WM_KEYDOWN:
        {
            bool alt = (::GetAsyncKeyState(VK_MENU) & 0x8000) != 0;

            switch (wParam)
            {
            case 'V':
                g_VSync = !g_VSync;
                break;
            case VK_ESCAPE:
                ::PostQuitMessage(0);
                break;
            case VK_RETURN:
                if ( alt )
                {
            case VK_F11:
                SetFullscreen(!g_Fullscreen);
                }
                break;
            }
        }
        break;
        // The default window procedure will play a system notification sound 
        // when pressing the Alt+Enter keyboard combination if this message is 
        // not handled.
        case WM_SYSCHAR:
        break;

        case WM_SIZE:
        {
            RECT clientRect = {};
            ::GetClientRect(g_handleWindow, &clientRect);

            int width = clientRect.right - clientRect.left;
            int height = clientRect.bottom - clientRect.top;

            Resize(width, height);
        }
        break;        
        
        case WM_DESTROY:
            ::PostQuitMessage(0);
            break;
        default:
            return ::DefWindowProcW(hwnd, message, wParam, lParam);
        }
    }
    else
    {
        return ::DefWindowProcW(hwnd, message, wParam, lParam);
    }

    return 0;
}

// Main Entry Point
int CALLBACK wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PWSTR lpCmdLine, int nCmdShow)
{
    // Windows 10 Creators update adds Per Monitor V2 DPI awareness context.
    // Using this awareness context allows the client area of the window 
    // to achieve 100% scaling while still allowing non-client window content to 
    // be rendered in a DPI sensitive fashion.
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // Window class name. Used for registering / creating the window.
    const wchar_t* windowClassName = L"DX12WindowClass";
    ParseCommandLineArguments();

    EnableDebugLayer();    

    g_TearingSupported = CheckTearingSupport();

    RegisterWindowClass(hInstance, windowClassName);
    g_handleWindow = CreateWindow(windowClassName, hInstance, L"Learning DirectX 12",
        g_ClientWidth, g_ClientHeight);

    // Initialize the global window rect variable.
    ::GetWindowRect(g_handleWindow, &g_WindowRectangle);
    
    ComPtr<IDXGIAdapter4> dxgiAdapter4 = GetAdapter(g_UseWarp);
    g_Device = CreateDevice(dxgiAdapter4);
    g_CommandQueue = CreateCommandQueue(g_Device, D3D12_COMMAND_LIST_TYPE_DIRECT);
    g_SwapChain = CreateSwapChain(g_handleWindow, g_CommandQueue, g_ClientWidth, g_ClientHeight, g_NumFrames);
    g_CurrentBackBufferIndex = g_SwapChain->GetCurrentBackBufferIndex();
    g_RTVDescriptorHeap = CreateDescriptorHeap(g_Device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, g_NumFrames);
    g_RTVDescriptorSize = g_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    UpdateRenderTargetViews(g_Device, g_SwapChain, g_RTVDescriptorHeap);

    // since there needs to be as many allocators as in flight render frames, one if created for each frame/# of swapchain backbuffers
    for (int i = 0; i < g_NumFrames; ++i)
    {
        g_CommandAllocators[i] = CreateCommandAllocator(g_Device, D3D12_COMMAND_LIST_TYPE_DIRECT);
    }
    g_CommandList = CreateCommandList(g_Device, g_CommandAllocators[g_CurrentBackBufferIndex], D3D12_COMMAND_LIST_TYPE_DIRECT);

    g_Fence = CreateFence(g_Device);
    g_FenceEvent = CreateEventHandle();

    g_IsInitialized = true;
    ::ShowWindow(g_handleWindow, SW_SHOW);

    MSG message = {};
    while (message.message != WM_QUIT)
    {
        if (::PeekMessage(&message, NULL, 0, 0, PM_REMOVE))
        {
            ::TranslateMessage(&message);
            ::DispatchMessage(&message);
        }
    }

    // Make sure the command queue has finished all commands before closing.
    Flush(g_CommandQueue, g_Fence, g_FenceValue, g_FenceEvent);

    ::CloseHandle(g_FenceEvent);

    return 0;
}