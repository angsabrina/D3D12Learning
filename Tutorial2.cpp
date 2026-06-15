#include <Tutorial2.h>

#include <Application.h>
#include <CommandQueue.h>
#include <Helpers.h>
#include <Window.h>

#include <wrl.h>
using namespace Microsoft::WRL;

#include <d3dx12.h>
#include <d3dcompiler.h>

#include <algorithm> // For std::min and std::max.
#if defined(min)
    #undef min
#endif

#if defined(max)
    #undef max
#endif

using namespace DirectX;

// Clamp a value between a min and max range.
template<typename T>
constexpr const T& clamp(const T& val, const T& min, const T& max)
{
    return val < min ? min : val > max ? max : val;
}

// Vertex data for a colored cube
struct VertexPosColor
{
    XMFLOAT3 position;
    XMFLOAT4 color;
};

// vertex data for cube mesh 
static VertexPosColor g_Vertices[8] = {
    { XMFLOAT3(-1.0f, -1.0f, -1.0f), XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f) }, // 0
    { XMFLOAT3(-1.0f,  1.0f, -1.0f), XMFLOAT4(0.0f, 1.0f, 0.0f, 1.0f) }, // 1
    { XMFLOAT3( 1.0f,  1.0f, -1.0f), XMFLOAT4(1.0f, 1.0f, 0.0f, 1.0f) }, // 2
    { XMFLOAT3( 1.0f, -1.0f, -1.0f), XMFLOAT4(1.0f, 0.0f, 0.0f, 1.0f) }, // 3
    { XMFLOAT3(-1.0f, -1.0f,  1.0f), XMFLOAT4(0.0f, 0.0f, 1.0f, 1.0f) }, // 4
    { XMFLOAT3(-1.0f,  1.0f,  1.0f), XMFLOAT4(0.0f, 1.0f, 1.0f, 1.0f) }, // 5
    { XMFLOAT3( 1.0f,  1.0f,  1.0f), XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f) }, // 6
    { XMFLOAT3( 1.0f, -1.0f,  1.0f), XMFLOAT4(1.0f, 0.0f, 1.0f, 1.0f) }  // 7
};

// points and lines cannot be used to generate a solid mesh, only triangles (and patches) can be used to generate a solid mesh.

// defines the index buffer that is used to represent triangles used to create the solid cube
static WORD g_Indicies[36] =
{
    0, 1, 2, 0, 2, 3,
    4, 6, 5, 4, 7, 6,
    4, 5, 1, 4, 1, 0,
    3, 2, 6, 3, 6, 7,
    1, 5, 6, 1, 6, 2,
    4, 0, 3, 4, 3, 7
};

// constructor passes the initialization variable to parent Game class
Tutorial2::Tutorial2( const std::wstring& name, int width, int height, bool vSync )
    : super(name, width, height, vSync)
    // mask out a rectangular region of the screen for rendering
    , m_ScissorRect(CD3DX12_RECT(0, 0, LONG_MAX, LONG_MAX)) 
    // specifies viewable part of the screen to render to. can be smaller than the size of the screen but should not be larger than the RT bound to the OM stage
    , m_Viewport(CD3DX12_VIEWPORT(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height))) 
    // vertical field of view of the camera. initialized to 45 can be adjusted using mouse wheel
    , m_FoV(45.0)
    , m_ContentLoaded(false)
{
}

// used to initialize vertex buffer and index buffer resources
void Tutorial2::UpdateBufferResource(
    ComPtr<ID3D12GraphicsCommandList2> commandList,                     // required to transfer buffer data to dest. resource
    ID3D12Resource** pDestinationResource,                              // pointers used to store destination and intermediate resources created
    ID3D12Resource** pIntermediateResource,                             // ""
    size_t numElements, size_t elementSize, const void* bufferData,     // CPU buffer data transferred to GPU resource
    D3D12_RESOURCE_FLAGS flags )                                        // used to specify additional flags required to create buffer resource
{
    auto device = Application::Get().GetDevice();

    size_t bufferSize = numElements * elementSize;

    // create a GPU resource large enough to store the buffer
    ThrowIfFailed(device->CreateCommittedResource(
        &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),              // pointer to properties for resource's heap
        D3D12_HEAP_FLAG_NONE,                                           // heap options
        &CD3DX12_RESOURCE_DESC::Buffer(bufferSize, flags),              // pointer to struct that describes resource
        D3D12_RESOURCE_STATE_COMMON,                                    // initial state of resource
        nullptr,                                                        // describes default value for clear color. must be NULL when used with D3D12_RESOURCE_DIMENSION_BUFFER
        IID_PPV_ARGS(pDestinationResource)));                           // [out] pointer to crated resource object, when NULL no object will be created and S_FALSE is returned

    // Create an committed resource for the upload.
    if (bufferData)
    {
        ThrowIfFailed(device->CreateCommittedResource(
            &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
            D3D12_HEAP_FLAG_NONE,
            &CD3DX12_RESOURCE_DESC::Buffer(bufferSize),
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(pIntermediateResource)));

        // CPU buffer data can be transffered to GPU resources
        D3D12_SUBRESOURCE_DATA subresourceData = {};
        subresourceData.pData = bufferData;                                 // ptr to memory block that contains subresource data
        subresourceData.RowPitch = bufferSize;                              // 
        subresourceData.SlicePitch = subresourceData.RowPitch;              //

        UpdateSubresources(commandList.Get(), 
            *pDestinationResource,                                          //
            *pIntermediateResource,                                         //
            0,                                                              //
            0,                                                              //
            1,                                                              //
            &subresourceData);                                              //
    }
}            