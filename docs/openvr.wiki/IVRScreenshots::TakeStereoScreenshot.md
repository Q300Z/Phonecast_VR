`EVRScreenshotError TakeStereoScreenshot( ScreenshotHandle_t *pOutScreenshotHandle, const char *pchPreviewFilename, const char *pchVRFilename )`

Request a stereoscopic screenshot.

* `ScreenshotHandle_t *pOutScreenshotHandle` - This is the handle returned for the screenshot that can be used in other calls when referring to this screenshot.
* `const char *pchPreviewFilename` - Full path of the preview of the screenshot image to be written to. This is a regular 2D image of the screenshot used in previews before showing the VR 3D image.
* `const char *pchVRFilename` - Full path of the screenshot image to be written to.  This will be in the format requested if available.

**Description**

 Tells the compositor to take an internal screenshot of type VRScreenshotType_Stereo. It will take the current submitted scene textures of the running application and write them into the preview image and a side-by-side file for the VR image. This is similiar to RequestScreenshot, but doesn't ever talk to the application, just takes the shot and submits.