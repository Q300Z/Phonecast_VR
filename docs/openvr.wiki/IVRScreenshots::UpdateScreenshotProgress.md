`EVRScreenshotError UpdateScreenshotProgress( ScreenshotHandle_t screenshotHandle, float flProgress )`

Present the user with a progress display during screenshot generation.

* `ScreenshotHandle_t screenshotHandle` - Handle to the screenshot that the filename is being requested. 
* `float flProgress` - A value between 0 and 1 indicating how far along the screenshot processing has been progressed.

**Description**

Call this if the application is taking the screen shot will take more than a few ms processing. This will result in an overlay being presented that shows a completion bar.  For example, stereo panoramic screenshots can take several seconds to render.  Typically the application is paused and is not submitting views to the compositor.
