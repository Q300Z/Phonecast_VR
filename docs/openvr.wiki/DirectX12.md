# DirectX12 support in SteamVR

SteamVR supports submission of eye buffers originating from the DirectX12 graphics API.  

## Image description

When submitting a DirectX12 image as an overlay or eye texture into SteamVR, use **TextureType_DirectX12** for its **ETextureType**. The handle to pass into the **Texture_t** structure should be a pointer to a **D3D12TextureData_t** structure containing the DirectX12 resources needed for the runtime to process it:

```c++
struct D3D12TextureData_t
{
	ID3D12Resource *m_pResource;
	ID3D12CommandQueue *m_pCommandQueue;
	uint32_t m_nNodeMask;
};
```
* m_pResource - pointer to **ID3D12Resource** created with **D3D12_RESOURCE_DIMENSION_TEXTURE2D**.
* m_pCommandQueue - pointer to **ID3D12CommandQueue** on which SteamVR will execute the transfer of the image.
* m_nNodeMask - in the case of multi-adapter, this is a bitmask of the set of nodes.  If not using multi-adapter, set this to 0.

## Image Resource State

The D3D12 **m_pResource** should be transitioned to the resource state **D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE** prior to passing it ot the SteamVR runtime.  The resource will remain in the **D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE** state when the submission work has finished executing.

## Explicit Timing
DirectX12 applications can enable explicit timing mode by calling **IVRCompositor::SetExplicitTimingMode**. In DirectX12, the purpose of explicit timing mode is to get a more accurate GPU timestamp for when the frame begins.

More accurate GPU timestamp for the start of the frame is achieved by the application calling **IVRCompositor::SubmitExplicitTimingData** immediately before its first submission to the **ID3D12CommandQueue** for the frame. This is more accurate because normally this GPU timestamp is recorded during WaitGetPoses. In D3D11, WaitGetPoses queues a GPU timestamp write, but it does not actually get submitted to the GPU until the application performs its next flush. By using SubmitExplicitTimingData, the timestamp is recorded at the same place for DirectX12 as it is for D3D11, resulting in a more accurate GPU time measurement for the frame.

## Example Code

A simple example of using DirectX12 with SteamVR on Windows (Visual Studio 2015+) can be found in the SDK [[https://github.com/ValveSoftware/openvr/tree/master/samples/hellovr_dx12]].  Please note that the example does not use [Explicit Timing](#explicit-timing) and is quite simplified compared to what a real application is likely to do.  This is because the rendering command list in the example application can be built in a very small amount of CPU time whereas a real application will have much more rendering work to generate.  In the example code, the sequence of timing in the frame is as follows:

* **IVRCompositor::WaitGetPoses** is called after presenting the companion window which implicitly calls **IVRCompositor::PostPresentHandoff** and updates the latest poses.  
* The application builds a command list that renders the left and right eye to two separate render targets.  The matrices for the HMD and other tracked devices from the most recent call to **IVRCompositor::WaitGetPoses** are used.  The command list also contains commands to render to the companion window.
* The command list is submitted to the queue using ExecuteCommandLists
* The left eye texture is submitted using **IVRCompositor::Submit**.
* The right eye texture is submitted using **IVRCompositor::Submit**.
* The application presents the companion window using the swapchain Present.
* Then the application goes back to the first step and repeats for the next frame.

A real application is likely to have more rendering work to do and thus may want to have command lists for the frame ready prior to **IVRCompositor::WaitGetPoses** returning.  Such an application will also need to use [Explicit Timing](#explicit-timing) to account for the GPU time gap between **IVRCompositor::WaitGetPoses** returning and GPU work for the frame starting.  A real application would enable [Explicit Timing](#explicit-timing) using **IVRCompositor::SetExplicitTimingMode** at startup and its update loop might look something like this:

* Build rendering command lists for the next frame prior to calling **IVRCompositor::WaitGetPoses** (or simultaneously on other threads).
* After **IVRCompositor::WaitGetPoses** returns, update the transforms in the previously recorded command lists with the latest poses.  For example, this could be done by using CopyBufferRegion to copy new poses into the constant buffer locations that were pointed to in the previously recorded command list.  The CB update command list(s) would then be submitted prior to submitting the rendering command list.  Another option would be to have the constant buffers be located in persistently mapped buffers and to update the buffer data with new poses prior to submission.
* Just before calling the first ExecuteCommandLists for the frame, call **IVRCompositor::SubmitExplicitTimingData** to mark the beginning of GPU work for the frame.
* Submit the rendering work for the frame using ExecuteCommandLists.
* Submit left and right eye using **IVRCompositor::Submit**.
* After presenting the companion window, call **IVRCompositor::PostPresentHandoff**.  Note that this call is optional, but may be useful if there is additional work the application wants to do prior to waiting for new poses.  If it is not called, it will be implicitly called by **IVRCompositor::WaitGetPoses**.
* Go back to the first step and start building command buffers again for the next frame.
