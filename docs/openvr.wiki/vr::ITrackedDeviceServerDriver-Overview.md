The `vr::ITrackedDeviceServerDriver` interface represents a single physical tracked device in the system. It is implemented in driver dynamic libs. 

Once an instance of this interface is provided to vrserver it should remain valid until Cleanup is called on the `ITrackedDeviceServerProvider` object.

**`HmdError Activate( uint32_t unObjectId )`**

This is called before tracked device is returned to the application. It will always be
called before any display or tracking methods. Memory and processor use by the
`vr::ITrackedDeviceServerDriver` object should be kept to a minimum until it is activated. 
The pose listener is guaranteed to be valid until Deactivate is called, but 
should not be used after that point.


**`void Deactivate()`**

This is called when vrserver is switching to another driver.


**`const char *GetId()`**

Returns the serial number of this particular tracked device. This value is opaque to the VR system itself,
but should be unique within the driver because it will be passed back in via FindTrackedDeviceDriver.


**`void DebugRequest( const char *pchRequest, char *pchResponseBuffer, uint32_t unResponseBufferSize )`**

A VR Client has made this debug request of the driver. The set of valid requests is entirely
up to the driver and the client to figure out, as is the format of the response. Responses that
exceed the length of the supplied buffer should be truncated and null terminated.

# vr::IVRDisplayComponent Methods

**`void GetWindowBounds( int32_t *pnX, int32_t *pnY, uint32_t *pnWidth, uint32_t *pnHeight )`**

SteamVR will call this function when the VR display is in extended mode (ie. a part of the desktop), which SteamVR determines by calling `IsDisplayOnDesktop`. The function should provide the size and position that the window needs to be on the desktop, such that it fills the extended display.

**`bool IsDisplayOnDesktop()`**

Returns true if the display is extending the desktop. We recommend you avoid this 'extended mode' and instead make a direct-mode HMD. Direct-mode requires setting the EDID VID & PID properties.


**`bool IsDisplayRealDisplay()`**

Returns true if the display is real and not a fictional display.

**`void GetRecommendedRenderTargetSize( uint32_t *pnWidth, uint32_t *pnHeight )`**

Provides the suggested size for the intermediate render target that the distortion pulls from.
This function allows you to suggest a render resolution to the application to produce a final, crisp image.

We suggest returning a resolution that results in 1:1 pixels with the display at the center of projection after the distortion function is applied. This maximizes the detail where the user looks the most, without rendering too many extra pixels. You can estimate the correct value using your own distortion measurements. At the center of projection, divide a small post-distortion pixel distance by its pre-distortion pixel distance, this will yield a good scaling value.

Let's walk through a brief example. We first need to locate the center of projection in U,V coordinates. The center for each eye is calculated horizontally as abs(left)/abs(left-right), and similarly for top/bottom. For this simple example, we will assume that the resulting U,V coordinates are 0.5, 0.5, but it is likely that in a real HMD, the center of projection is not at the exact center of the viewport. Next, `ComputeDistortion( EVREye_Eye_Left, 0.5f, 0.5f )` returns a green channel U,V of `(0.5, 0.5)`, and `ComputeDistortion( EVREye_Eye_Left, 0.52f, 0.50f )` returns `(0.528, 0.5)`, so the X delta is 0.02 and the U delta is 0.028. 0.028 / 0.02 = 1.4, or a scaling factor of 140%. Therefore, the pnWidth and pnHeight values returned by this function should be 140% of the display panel's actual resolution. Note that the scaling may not be identical in both axis, and an exact scaling factor isn't strictly necessary.

These width and height values set the "100%" value in the user's settings menu, so it's nice to pick values that look reasonable to the user. Several factors will affect the actual rendered resolution. SteamVR performs a performance benchmark to estimate an appropriate tradeoff between resolution and performance. A user can adjust the resolution in the settings menu. An application can also choose to render at a different resolution. There's no guarantee that the values returned by your driver will actually be used.

**`void GetProjectionRaw( Hmd_Eye eEye, float *pfLeft, float *pfRight, float *pfTop, float *pfBottom )`**

The components necessary to build an application's projection matrix. The values represent the tangents of the half-angles from the center view axis. 

Note that “bottom” and “top” are backwards. “Bottom” is the tan angle from the center of projection to the top (+Y) edge of the display, and “top” is the center of projection to the bottom (-Y) edge of the display.

In a simple example, if your HMD has an FOV of 90 degrees in the vertical and horizontal axes, then it has a 45 degree angle between the forward vector and each side. The values would be `tan(45 degrees)` which is 1.0,  and their signs would be left and top negative, such that:
```
*pfLeft = -1.0;
*pfRight = 1.0;
*pfTop = -1.0;
*pfBottom = 1.0;
```

It is typical for HMDs to want to provide a larger field of view in the periphery of each eye and less in the stereo overlap. In this second example, the lens is mounted off-center of the display panel to create the desired peripheral view angle:

![image](https://user-images.githubusercontent.com/3059423/200420697-43ddd135-c5a8-4e47-b788-e0400bd8cfb5.png)

(The lens is drawn as being symmetrical, but in reality it probably wouldn't be). 

Using the above values, we’d return something like:
Left = -2.747, Right = 1.732, Top = -1.42, Bottom = 1.42 (assuming the lens is vertically centered, but lower fov).

These are tan angles for the edges of the display as seen through the lens – i.e. measured on the eye side.

Since there is variation in assembly technique, typically these values are calibrated per-unit in the factory. These differences also affect the distortion curves, so those are captured at the same time on a per-unit basis.

When an application submits a frame, it may tell SteamVR about the field of view it used. If the application rendered with a larger FoV than the driver gave through `GetProjectionRaw`, then the image will be cropped to the driver's FoV. If the application rendered with a smaller FoV, then the image will be inset such that 0..1 UV always maps to the driver's `GetProjectionRaw` values. If an application uses an FoV other than the one specified by GetProjectinRaw, it must communicate this via IVRCompositor::Submit by specifying the projection used via vr::VRTextureWithDepth_t or vr::VRTextureWithPoseAndDepth_t. If the application does not, the output will look incorrect in headset as it will be improperly stretched or compressed.

**`DistortionCoordinates_t ComputeDistortion( Hmd_Eye eEye, float fU, float fV )`**

Returns the result of the distortion function for the specified eye and input UVs. UVs go from 0,0 in the upper left of eye's viewport and 1,1 in the lower right of the eye's viewport.

If you were to use an identity function for your distortion (i.e. r={u,v}, g={u,v}, b={u,v}), the image will not be distorted by the compositor. The rendered image from the application will be mapped to the GetEyeOutputViewport. This function is not used when drivers implement IVRDriverDirectModeComponent, so an identity function can be used. IVRDriverDirectModeComponent should be used when you are using your own compositor and distortion processing.

### Usage in the context of a VR Game:
* When SteamVR starts, the compositor will gather distortion data. ComputeDistortion will be called several thousand times to sample the distortion space.
* The user launches a game...
* Before the first frame, the game will ask SteamVR what size render target is recommended. GetRecommendedRenderTargetSize will be used here. The user may override the value passed to the game through the SteamVR settings menu, and the game may chose a different render resolution for its own reasons.
* Each Frame, the game queries for updated pose information for placing cameras in game. The camera pose is created by combining poses from TrackedDevicePoseUpdated and GetHeadFromEyePose. The game will also call GetProjectionRaw to render with the correct FoV.

**`void GetEyeOutputViewport( Hmd_Eye eEye, uint32_t *pnX, uint32_t *pnY, uint32_t *pnWidth, uint32_t *pnHeight )`**

Describes the viewport in the frame buffer to draw the output of the distortion into. SteamVR will call this function for each eye.

GetEyeOutputViewport’s X and Y specifies the upper-left location of the eye viewport in display space. If the display is rotated such that the upper-left is not actually in the upper-left, that additional transform needs to be specified in the property compositor residual. Do not try to use negative height, flipping width and height, or trying to compensate using ComputeDistortion, instead use a custom HeadFromEye matrix.

## Controlling HeadFromEye Matrix

An application can find the correct position for its virtual cameras using the head tracking pose, head from eye pose, and projection matrices. For example: `LeftCamera = LeftProjectionFromEye * LeftEyeFromHead * HeadFromStanding`. Where do these components come from?

LeftProjectionFromEye is the projection from GetProjectionRaw.
LeftEyeFromHead will be discussed below.
HeadFromStanding is the pose from IVRServerDriverHost::TrackedDevicePoseUpdated for the HMD.

Note: When dealing with matrices, the order in which you write the operands is affected by whether the matrix is written column-major or row-major, and the direction of poses (ie. from/to), so be sure to double check that you have inverted matrices when necessary and that the order of multiplication is correct.

There are two methods to control the pose of the eye, which sets both the location and direction of the application's cameras. This pose can be modified to account for interesting scenarios like rotated or canted displays.


An application shouldd call GetHeadFromEyePose every frame, the definition will be repeated here but this **is not** a function implemented by your driver:

**`HmdMatrix34_t GetHeadFromEyePose( Hmd_Eye eEye )`**

Returns the eye pose relative to the head, including adjustments for IPD. Given that +X is to the right, the left eye should be a pose with Tx = -ipd/2, and the right eye should be a pose with Tx = +ipd/2.

There are two distinct, incompatible paths to controlling the output of GetHeadFromEyePose.

**Method 1: IPD Property**
Whenever you change the IPD property `Prop_UserIpdMeters_Float`, SteamVR will recompute the HeadFromEyePose matrix and will give the new matrix to the application from then on. The matrix contains only the simple translation of the eyes based on IPD in the X-direction, no other movement, rotation, or scaling* is performed. 

*SteamVR properties allow users to modify some of these settings at their own risk

**Method 2: Call IVRServerDriverHost::SetDisplayEyeToHead**
The method `IVRServerDriverHost::SetDisplayEyeToHead` allows a driver to provide its own vr::HmdMatrix34_t for the left and right eyes. These matrices will be provided as-is to applications that call GetHeadFromEyePose. Once you call `SetDisplayEyeToHead`, SteamVR will no longer automatically update the HeadFromEyePose based on changes to the property `Prop_UserIpdMeters_Float`, it is now the drivers responsibility to handle all IPD changes. There is no mechanism to re-enable automatic IPD handling.

For futher information about poses and matrices in SteamVR, please see the page [[Matrix Usage Example]].

# Assorted capability methods

**`TrackedDeviceDriverInfo_t GetTrackedDeviceDriverInfo()`**

Returns all the static information that will be reported to the clients.
	

# Administrative Methods

**`const char *GetModelNumber()`**

Returns the model number of this tracked device.

**`const char *GetSerialNumber()`**

Returns the serial number of this tracked device.

# IPD Methods

**`float GetIPD()`**

Gets the current IPD (Interpupillary Distance) in meters.

# Tracking Methods

**`DriverPose_t GetPose()`**

Gets the initial pose of the device. Future poses should be provided via `IServerDriverHost::TrackedDevicePoseUpdated()` function.


# Property Methods

**`bool GetBoolTrackedDeviceProperty( TrackedDeviceProperty prop, TrackedPropertyError *pError )
float GetFloatTrackedDeviceProperty( TrackedDeviceProperty prop, TrackedPropertyError *pError ) = 0;
int32_t GetInt32TrackedDeviceProperty( TrackedDeviceProperty prop, TrackedPropertyError *pError ) = 0;
uint64_t GetUint64TrackedDeviceProperty( TrackedDeviceProperty prop, TrackedPropertyError *pError ) = 0;
uint32_t GetStringTrackedDeviceProperty( TrackedDeviceProperty prop, char *pchValue, uint32_t unBufferSize, TrackedPropertyError *pError )`**

Returns a property of the appropriate type.


# Controller Methods


**`VRControllerState_t GetControllerState()`**

Returns the initial state of a controller.


**`bool TriggerHapticPulse( uint32_t unAxisId, uint16_t usPulseDurationMicroseconds )`**

Returns a uint64 property. If the property is not available this function will return 0.
