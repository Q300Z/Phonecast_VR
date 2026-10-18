# This interface had been deprecated and is no longer recommended for new projects.

The `IVRIOBuffer` interface supports creating, reading from, and writing to a fixed-element-size cross-process, globally-named ring-buffer with extremely low synchronization overhead between multiple readers and a single writer. This interface is useful whenever there is very high-frequency of high-bandwidth data stream where it is acceptable for readers to fall behind and even miss elements in the stream, as this allows the writer to use a ring-buffer instead of an ever-growing buffer, and the writer can avoid writing if there are no readers.

An example of a high-frequency `IVRIOBuffer` stream is `vr::ImuSample_t` data arriving at 1000Hz from an HMD or at 250-500Hz from a tracked controller. In SteamVR, lighthouse-based HMDs and lighthouse-based tracked controllers expose a >= 512-element ring-buffer of IMU `vr::ImuSample_t` data under the name `"/devices/lighthouse/<serial-number>/imu"` with an element size of `sizeof(vr::ImuSample_t)`. A driver or an application can read the IMU data as follows:

```cpp
/* initialization */
vr::IOBufferHandle_t ulIMUStream;
vr::VR_Init(); // not necessary from an openvr driver
vr::VRIOBuffer()->Open( "/devices/lighthouse/LHR-D85D0087/imu", vr::IOBufferMode_Read, sizeof(vr::ImuSample_t), 0, &ulIMUStream );

/* whenever you want to process IMU data */
vr::ImuSample_t imuSamples[ 10 ];
uint32_t unRead;

while ( vr::VRIOBuffer()->Read( ulIMUStream, imuSamples, sizeof(imuSamples), &unRead ) == vr::IOBuffer_Success && unRead > 0 )
{
    for ( uint32_t i = 0; i < unRead/sizeof(vr::ImuSample_t); i++ )
    {
        // process up to 10 samples at a time, you don't need to drain the available
        // samples if you don't want to
    }
}

/* when you’re done */
vr::VRIOBuffer()->Close( ulIMUStream );
```

A driver or other application that wants to create its own `IVRIOBuffer` stream of data must specify the `vr::IOBufferMode_Create | vr::IOBufferMode_Write` flags to `IVRIOBuffer::Open` and provide the number of elements for the created ring-buffer:


```cpp
/* initialization */
vr::IOBufferHandle_t ulMyStream;
vr::VRIOBuffer()->Open( "/my/custom/path", (vr::EIOBufferMode)(vr::IOBufferMode_Create|vr::IOBufferMode_Write), sizeof(MyDataStructure), 512, &ulMyStream );

/* write data whenever you like */
MyDataStructure myStruct;
vr::VRIOBuffer()->Write( ulMyStream, &myStruct, sizeof(MyDataStructure) );

/* when you're done */
vr::VRIOBuffer()->Close( ulMyStream );
```

An example of an appropriate high-bandwidth `IVRIOBuffer` stream might be a stream of raw camera image data which requires 5MP per frame. A writer might create a 3-element fixed-size ring-buffer which multiple consumers could read from:

```cpp
vr::IOBufferHandle_t ulCameraFrames;
vr::VRIOBuffer()->Open( "/path/to/my/camera/frames", (vr::EIOBufferMode)(vr::IOBufferMode_Create|vr::IOBufferMode_Write), 5*1024*1024*3, 3, &ulCameraFrames );
```
