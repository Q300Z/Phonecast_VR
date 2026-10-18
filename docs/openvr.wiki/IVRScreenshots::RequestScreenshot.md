`EVRScreenshotError RequestScreenshot( ScreenshotHandle_t *pOutScreenshotHandle, EVRScreenshotType type, const char *pchPreviewFilename, const char *pchVRFilename )`

* `ScreenshotHandle_t *pOutScreenshotHandle` - This is the handle returned for the screenshot that can be used in other calls when referring to this screenshot.
* `EVRScreenshotType type` - The format type of the screenshot being requested. A request of the VRScreenshotType_Stereo type will always work. Other types will depend on the underlying application support.
* `const char *pchPreviewFilename` - Full path of the preview of the screenshot image to be written to. This is a regular 2D image of the screenshot used in previews before showing the VR 3D image.
* `const char *pchVRFilename` - Full path of the screenshot image to be written to.  This will be in the format requested if available.  Current formats:
VRScreenshotType_Mono: the VR filename is ignored (can be nullptr), this is a normal flat single shot.
VRScreenshotType_Stereo:  The VR image should be a side-by-side with the left eye image on the left.
VRScreenshotType_Cubemap: The VR image should be six square images composited horizontally.
VRScreenshotType_StereoPanorama: above/below with left eye panorama being the above image.  Image is typically square with the panorama being 2x horizontal.

**Description**


Initiates a screenshot request of the specified type and placing the generated screenshot and its preview into the destination files as named.

If a screenshot is already in progress this will return VRScreenshotError_ScreenshotAlreadyInProgress and a new screenshot will not be initiated.

Note that the VR dashboard will call this function when the user presses the screenshot binding (currently System
Button + Trigger).  If Steam is running, the destination file names will be in %TEMP% and will be copied into
Steam's screenshot library for the running application once SubmitScreenshot() is called.
If Steam is not running, the paths will be in the user's documents folder under Documents\SteamVR\Screenshots.

Other VR applications can call this to initiate a screenshot outside of user control. The destination file names do not need an extension, will be replaced with the correct one for the format which is currently PNG.

