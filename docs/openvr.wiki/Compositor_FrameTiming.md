`Compositor_FrameTiming` contains timing information for a single frame.

Frame timing can be retrieved either for a single frame by specifying how many frames ago to retrieve information for through `IVRCompositor::GetFrameTiming`, or for multiple frames from the current frame with `IVRCompositor::GetFrameTimings`.

Information for the last 128 frames are stored. Attempts to pass larger arrays will only be filled up to this limit.

To maintain compatibility with previous versions, `Compositor_FrameTiming` **must** be set to the size of the struct being passed. For C#, this can be done with:

```c#
(uint)System.Runtime.InteropServices.Marshal.SizeOf(typeof(Compositor_FrameTiming));
```

The following are descriptions for each member of `Compositor_FrameTiming`:

	uint32_t m_nSize - Set to sizeof( Compositor_FrameTiming )
	uint32_t m_nFrameIndex - Incremented each frame and is the specified index for the frame
	uint32_t m_nNumFramePresents - Number of times this frame was presented, in case of reprojection
	uint32_t m_nNumMisPresented - Number of times this frame was presented on a vsync other than it was originally predicted to
	uint32_t m_nNumDroppedFrames - Number of additional times previous frame was scanned out
	uint32_t m_nReprojectionFlags - Contains information on reprojection, if applicable. See below for possible options

	double m_flSystemTimeInSeconds - Absolute time reference for comparing frames. This aligns with the vsync that running start is relative to

The following may include work from other processes due to OS scheduling. The fewer packets of work these are broken up into, the less likely this will happen.  
GPU work can be broken up by calling Flush.  This can sometimes be useful to get the GPU started processing that work earlier in the frame.

	float m_flPreSubmitGpuMs - Time spent rendering the scene (gpu work submitted between WaitGetPoses and second Submit)
	float m_flPostSubmitGpuMs - Additional time spent rendering by application (e.g. companion window)
	float m_flTotalRenderGpuMs - Time between work submitted immediately after present (ideally vsync) until the end of compositor submitted work
	float m_flCompositorRenderGpuMs - Time spend performing distortion correction, rendering chaperone, overlays, etc.
	float m_flCompositorRenderCpuMs - Time spent on cpu submitting the above work for this frame
	float m_flCompositorIdleCpuMs - Time spent waiting for running start (application could have used this much more time)

Miscellaneous measured intervals

	float m_flClientFrameIntervalMs - Time between calls to WaitGetPoses
	float m_flPresentCallCpuMs Time blocked on call to present (usually 0.0, but can go long)
	float m_flWaitForPresentCpuMs - Time spent spin-waiting for frame index to change (not near-zero indicates wait object failure)
	float m_flSubmitFrameMs - Time spent in IVRCompositor::Submit (not near-zero indicates driver issue)

These are all relative to this frame's SystemTimeInSeconds

	float m_flWaitGetPosesCalledMs
	float m_flNewPosesReadyMs
	float m_flNewFrameReadyMs - Second call to IVRCompositor::Submit
	float m_flCompositorUpdateStartMs
	float m_flCompositorUpdateEndMs
	float m_flCompositorRenderStartMs

Other

	vr::TrackedDevicePose_t m_HmdPose - Pose used by app to render this frame

	uint32_t m_nNumVSyncsReadyForUse
	uint32_t m_nNumVSyncsToFirstView


**m_nReprojectionFlags** - A bit flag field that contains the possible options of:

	VRCompositor_ReprojectionReason_Cpu = 0x01
	VRCompositor_ReprojectionReason_Gpu = 0x02
	VRCompositor_ReprojectionAsync = 0x04 - Indicates the async reprojection mode is active, but does not indicate if reprojection actually happened or not. Use the ReprojectionReason flags above to check if reprojection was actually applied (i.e. scene texture was reused). NumFramePresents > 1 also indicates the scene texture was reused, and also the number of times that it was presented in total.
	VRCompositor_ReprojectionMotion = 0x08 - This flag indicates whether or not motion smoothing was triggered for this frame
	VRCompositor_PredictionMask = 0xF0 - The runtime may predict more than one frame (up to four) ahead if it detects the application is taking too long to render. These two bits will contain the count of additional frames (normally zero). Use the VR_COMPOSITOR_ADDITIONAL_PREDICTED_FRAMES macro to read from the latest frame timing entry.
	VRCompositor_ThrottleMask = 0xF00 - Number of frames the compositor is throttling the application. Use the VR_COMPOSITOR_NUMBER_OF_THROTTLED_FRAMES macro to read from the latest frame timing entry.

## Example usage
`Compositor_FrameTiming` can be used for calculating the current framerate of the application.
```c#
Compositor_FrameTiming currentFrame = new Compositor_FrameTiming();
Compositor_FrameTiming previousFrame = new Compositor_FrameTiming();
currentFrame.m_nSize = (uint)System.Runtime.InteropServices.Marshal.SizeOf(typeof(Compositor_FrameTiming));
previousFrame.m_nSize = (uint)System.Runtime.InteropServices.Marshal.SizeOf(typeof(Compositor_FrameTiming));
OpenVR.Compositor.GetFrameTiming(ref currentFrame, 0);
OpenVR.Compositor.GetFrameTiming(ref previousFrame, 1);

uint currentFrameIndex = currentFrame.m_nFrameIndex;
uint amountOfFramesSinceLast = currentFrameIndex - LastFrameSampleIndex;

double gpuFrametimeMs = 0;
double cpuFrametimeMs = 0;
double totalFrametimeMs = 0;

for (uint i = 0; i < amountOfFramesSinceLast; i++)
{
    OpenVR.Compositor.GetFrameTiming(ref currentFrame, i);
    OpenVR.Compositor.GetFrameTiming(ref previousFrame, i + 1);

    gpuFrametimeMs += currentFrame.m_flTotalRenderGpuMs;
    cpuFrametimeMs += currentFrame.m_flNewFrameReadyMs - currentFrame.m_flNewPosesReadyMs + currentFrame.m_flCompositorRenderCpuMs;
    totalFrametimeMs += (currentFrame.m_flSystemTimeInSeconds - previousFrame.m_flSystemTimeInSeconds) * 1000f;
}

gpuFrametimeMs /= amountOfFramesSinceLast;
cpuFrametimeMs /= amountOfFramesSinceLast;
totalFrametimeMs /= amountOfFramesSinceLast;

LastFrameSampleIndex = currentFrameIndex;

Information.GpuFrametime = (float) gpuFrametimeMs;
Information.CpuFrametime = (float) cpuFrametimeMs;
Information.TotalFrametime = (float) totalFrametimeMs;
Information.Framerate = Mathf.Max(0, Mathf.Min(targetRefreshRate, (int)(1f / totalFrametimeMs * 1000f)));
Information.MaxFrametime = targetRefreshRateMs;
Information.MaxFramerate = targetRefreshRate;
```