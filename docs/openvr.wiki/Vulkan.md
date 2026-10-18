# Vulkan support in SteamVR

SteamVR supports submission of eye buffers originating from the Vulkan graphics API. Due to the explicit nature of Vulkan, a bit of extra consideration is needed.

## Extensions

The SteamVR runtime will use the application's graphics queue to transmit textures to the SteamVR compositor. Depending on the platform, hardware and software version, it will need specific extensions enabled on the client application's instance and device. There are two Vulkan-specific methods in the IVRCompositor interface that can be used to query which extensions are required to enable:

```c++
	/** [Vulkan Only]
	* return 0. Otherwise it returns the length of the number of bytes necessary to hold this string including the trailing
	* null.  The string will be a space separated list of-required instance extensions to enable in VkCreateInstance */
	virtual uint32_t GetVulkanInstanceExtensionsRequired( VR_OUT_STRING() char *pchValue, uint32_t unBufferSize ) = 0;

	/** [Vulkan only]
	* return 0. Otherwise it returns the length of the number of bytes necessary to hold this string including the trailing
	* null.  The string will be a space separated list of required device extensions to enable in VkCreateDevice */
	virtual uint32_t GetVulkanDeviceExtensionsRequired( VkPhysicalDevice_T *pPhysicalDevice, VR_OUT_STRING() char *pchValue, uint32_t unBufferSize ) = 0;
```

The extensions to enable need to match the SteamVR runtime software and will change as SteamVR gets updated, so your application needs to make sure to call this at runtime and use the output result to enable extensions at instance and device creation time. **This is important, as just copying the result of these calls once into your application will cause it to break in the future when SteamVR gets updated with a newer version of its Vulkan submission code**.

**Note:** The extension list reported in `pchValue` is guaranteed to be available for `pPhysicalDevice`. The application **must** provide all extensions reported at device creation time, and **must not** re-filter the output.

## Image description

When submitting a Vulkan image as an overlay or eye texture into SteamVR, use **TextureType_Vulkan** for its **ETextureType**. The handle to pass into the **Texture_t** structure should be a pointer to a **VRVulkanTextureData_t** structure containing an explicit description of the image as well as the Vulkan resources needed for the runtime to process it:

```c++
struct VRVulkanTextureData_t
{
	uint64_t m_nImage; // VkImage
	VkDevice_T *m_pDevice;
	VkPhysicalDevice_T *m_pPhysicalDevice;
	VkInstance_T *m_pInstance;
	VkQueue_T *m_pQueue;
	uint32_t m_nQueueFamilyIndex;
	uint32_t m_nWidth, m_nHeight, m_nFormat, m_nSampleCount;
};
```

## Concurrency

Vulkan requires that only a single thread access a VkQueue at a time.  When any Vulkan Texture_t is passed to the runtime (through **IVRCompositor::Submit** or otherwise), the runtime will schedule work to the Vulkan queue represented by **m_pQueue**. As such, no other thread of your application should try to access that queue until the call returns.

In addition to **IVRCompositor::Submit**, the following functions may also access the queue:
 * **IVRCompositor::PostPresentHandoff**
 * **IVRCompositor::WaitGetPoses** (does not need to access the queue if using Explicit Timing and PostPresentHandoff, see [Explicit Timing](#explicit-timing))
 * **IVRCompositor::SubmitExplicitTimingData** (see [Explicit Timing](#explicit-timing))


## Image layout

Any Vulkan image represented by **m_nImage** should be in the **VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL** layout when passed to the SteamVR runtime, and will still be in that state when the submission work has finished executing.

## Image usage flags

Any Vulkan image represented by **m_nImage** must have been created with at least the following VkImageUsageFlags: **VK_IMAGE_USAGE_TRANSFER_SRC_BIT** | **VK_IMAGE_USAGE_SAMPLED_BIT**.

## Image Formats

The following image formats are currently supported for the **m_nFormat** of Vulkan Texture_t (through **IVRCompositor::Submit** or otherwise):

```c++
VK_FORMAT_R8G8B8A8_UNORM
VK_FORMAT_R8G8B8A8_SRGB
VK_FORMAT_B8G8R8A8_UNORM
VK_FORMAT_B8G8R8A8_SRGB
VK_FORMAT_R32G32B32A32_SFLOAT
VK_FORMAT_R32G32B32_SFLOAT
VK_FORMAT_R16G16B16A16_SFLOAT
VK_FORMAT_A2R10G10B10_UINT_PACK32
```

## Explicit Timing
Vulkan applications can enable explicit timing mode by calling **IVRCompositor::SetExplicitTimingMode**.  There are two purposes for SetExplicitTimingMode:
1. To get a more accurate GPU timestamp for when the frame begins.
2. (Optional) To avoid having WaitGetPoses access the Vulkan queue so that the queue can be accessed from another thread while WaitGetPoses is executing.

More accurate GPU timestamp for the start of the frame is achieved by the application calling **IVRCompositor::SubmitExplicitTimingData** immediately before its first submission to the Vulkan queue for the frame.  This is more accurate because normally this GPU timestamp is recorded during WaitGetPoses.  In D3D11, WaitGetPoses queues a GPU timestamp write, but it does not actually get submitted to the GPU until the application performs its next flush.  By using SubmitExplicitTimingData, the timestamp is recorded at the same place for Vulkan as it is for D3D11, resulting in a more accurate GPU time measurement for the frame.

Avoiding WaitGetPoses accessing the Vulkan queue can be achieved using SetExplicitTimingMode as well.  If this is desired, the application must call PostPresentHandoff itself prior to WaitGetPoses.  If SetExplicitTimingMode is true and the application calls PostPresentHandoff, then WaitGetPoses is guaranteed not to access the queue.  Note that PostPresentHandoff and SubmitExplicitTimingData will access the queue, so only WaitGetPoses becomes safe for accessing the queue from another thread.

## Example Code
A simple example of using Vulkan with SteamVR on Windows/Linux can be found in the SDK [[https://github.com/ValveSoftware/openvr/tree/master/samples/hellovr_vulkan]].  Please note that the example does not use [Explicit Timing](#explicit-timing) and is quite simplified compared to what a real application is likely to do.  This is because the rendering command buffer in the example application can be built in a very small amount of CPU time whereas a real application will have much more rendering work to generate.  In the example code, the sequence of timing in the frame is as follows:

* **IVRCompositor::WaitGetPoses** is called after presenting the companion window which implicitly calls **IVRCompositor::PostPresentHandoff** and updates the latest poses.  
* The application builds a command buffer that renders the left and right eye to two separate framebuffers.  The matrices for the HMD and other tracked devices from the most recent call to **IVRCompositor::WaitGetPoses** are used.  The command buffer also contains commands to render to the companion window.
* The command buffer is submitted to the Vulkan queue using vkQueueSubmit.
* The left eye texture is submitted using **IVRCompositor::Submit**.
* The right eye texture is submitted using **IVRCompositor::Submit**.
* The application presents the companion window using **vkQueuePresentKHR** .
* Then the application goes back to the first step and repeats for the next frame.

A real application is likely to have more rendering work to do and thus may want to have command buffers for the frame ready prior to **IVRCompositor::WaitGetPoses** returning.  Such an application will also need to use [Explicit Timing](#explicit-timing) to account for the GPU time gap between **IVRCompositor::WaitGetPoses** returning and GPU work for the frame starting.  A real application would enable [Explicit Timing](#explicit-timing) using **IVRCompositor::SetExplicitTimingMode** at startup and its update loop might look something like this:

* Build rendering command buffers for the next frame prior to calling **IVRCompositor::WaitGetPoses** (or simultaneously on other threads).
* After **IVRCompositor::WaitGetPoses** returns, update the transforms in the previously recorded command buffers with the latest poses.  For example, this could be done by using vkCmdCopyBuffer to copy new poses into the uniform buffer locations that were pointed to in the previously recorded command buffer.  The uniform update command buffer(s) would then be submitted prior to submitting the rendering command buffers.  Another option would be to have the uniform buffers be located in persistently mapped buffers and to update the buffer data with new poses prior to submission.
* Just before calling the first **vkQueueSubmit** for the frame, call **IVRCompositor::SubmitExplicitTimingData** to mark the beginning of GPU work for the frame.
* Submit the rendering work for the frame using **vkQueueSubmit**.
* Submit left and right eye using **IVRCompositor::Submit**.
* After presenting the companion window, call **IVRCompositor::PostPresentHandoff**.  Note that this call is optional, but may be useful if there is additional work the application wants to do prior to waiting for new poses.  If it is not called, it will be implicitly called by **IVRCompositor::WaitGetPoses**.
* Go back to the first step and start building command buffers again for the next frame.

