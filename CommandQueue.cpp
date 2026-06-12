#include "CommandQueue.h"

// A refactoring of the command queue code originally in main.cpp

CommandQueue::CommandQueue(Microsoft::WRL::ComPtr<ID3D12Device2> device, D3D12_COMMAND_LIST_TYPE type)
    : m_FenceValue(0)
    , m_CommandListType(type)
    , m_d3d12Device(device)
{
    D3D12_COMMAND_QUEUE_DESC desc = {};
    desc.Type = type;
    desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    desc.NodeMask = 0;

    ThrowIfFailed(m_d3d12Device->CreateCommandQueue(&desc, IID_PPV_ARGS(&m_d3d12CommandQueue)));
    ThrowIfFailed(m_d3d12Device->CreateFence(m_FenceValue, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_d3d12Fence)));

    m_FenceEvent = ::CreateEvent(NULL, FALSE, FALSE, NULL);
    assert(m_FenceEvent && "Failed to create fence event handle.");
}

// Create Command Allocators
// the backing memory for a command list, can only be used by 1 cmd list at a time but can be reused after the commands on that list have been executed by GPU
// a fence is used to check if the GPU cmd have finished executing.
Microsoft::WRL::ComPtr<ID3D12CommandAllocator> CommandQueue::CreateCommandAllocator()
{
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> commandAllocator;
    // types of cmd allocators: [direct (doesn't inherit GPU state), bundle (inherits all GPU state), compute, copy]
    ThrowIfFailed(m_d3d12Device->CreateCommandAllocator(m_CommandListType, IID_PPV_ARGS(&commandAllocator)));

    return commandAllocator;
}

// Create Command List
// for D3D12 execution of cmd recorded into a cmd list are ALWAYS deferred. ie invoking draw/dispatch cmds are not executed until the cmd list is sent to cmd queue
// cmd list can be reused immediately after it has been executed on cmd queue
Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList2> CommandQueue::CreateCommandList(
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator)
{
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList2> commandList;
    ThrowIfFailed(m_d3d12Device->CreateCommandList(
        0,                              // for single GPU operation 0, for multiple GPU nodes set bit to identify node
        m_CommandListType,              // type: [direct, bundle, compute, copy]
        allocator.Get(),                // pointer to cmd allocator that devices creates cmd list from
        nullptr,                        // pointer to pipeline state object with initial pipeline state for cmd list. NULL sets dummy PS so that drivers don't have undefined state
        IID_PPV_ARGS(&commandList)));   // [out] pointer access cmd list

    return commandList;
}

// returns a cmd list that can be directly used to issue GPU draw/dispatch cmds. 
// since there's no way to directly query for the cmd allocator used to reset the cmd list, a ptr to the cmd allocator is stored in private data space of the cmd list
Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList2> CommandQueue::GetCommandList()
{
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> commandAllocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList2> commandList;

    // before the cmd list can be reset, need an unused cmd allocator
    if ( !m_CommandAllocatorQueue.empty() && IsFenceComplete(m_CommandAllocatorQueue.front().fenceValue))
    {
        commandAllocator = m_CommandAllocatorQueue.front().commandAllocator;
        m_CommandAllocatorQueue.pop();

        ThrowIfFailed(commandAllocator->Reset());
    }
    else
    {
        commandAllocator = CreateCommandAllocator();
    }

    // cmd list queue is checked to see if there's any cmd lists in queue, otherwise create new one
    if (!m_CommandListQueue.empty())
    {
        commandList = m_CommandListQueue.front();
        m_CommandListQueue.pop();

        ThrowIfFailed(commandList->Reset(commandAllocator.Get(), nullptr));
    }
    else
    {
        commandList = CreateCommandList(commandAllocator);
    }

    // cmd allocator needs to be associated with cmd list
    // when assigning a COM object to private data of a D3D12Object using SetPrivateDataInterface, the internal ref counter of the COM object is incremented.
    ThrowIfFailed(commandList->SetPrivateData(__uuidof(ID3D12CommandAllocator), sizeof(Microsoft::WRL::ComPtr<ID3D12CommandAllocator>), commandAllocator.GetAddressOf()));

    return commandList;
}

// used to execute a cmd list that was previously retrieved using the GetCommandList() method.
uint64_t CommandQueue::ExecuteCommandList(Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList2> commandList)
{
    // before execution, cmd list must be closed
    commandList->Close();

    ID3D12CommandAllocator* commandAllocator;
    UINT dataSize = sizeof(commandAllocator);
    // retrieve a COM ptr of a COM object associated with the private data will also increment the ref counter of that COM obj
    ThrowIfFailed(commandList->GetPrivateData(__uuidof(ID3D12CommandAllocator), &dataSize, &commandAllocator));

    ID3D12CommandList* const tempCommandLists[] = {
        commandList.Get()
    };

    // ExecuteCommandLists expects an array of D3D12CommandList so a temp array is created.
    m_d3d12CommandQueue->ExecuteCommandLists(1, tempCommandLists);
    uint64_t fenceValue = Signal(); // the queue is signaled so the cmd allocators can be reused

    // allocator and lists are pushed to their respective queues to be reused the next time GetCommandList() is called
    m_CommandAllocatorQueue.emplace(CommandAllocatorEntry{ fenceValue, commandAllocator });
    m_CommandListQueue.push(commandList);

    commandAllocator->Release(); // release decreases the ref counter of COM ptr 

    return fenceValue; // fence value that can be used to perform CPU/GPU synchronization is returned to calling function
}