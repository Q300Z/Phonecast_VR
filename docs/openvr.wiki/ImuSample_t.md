Raw Interial Measurement Unit (IMU) data arrives in the form of `vr::ImuSample_t` data structures:

```cpp
  enum Imu_OffScaleFlags
  {
    OffScale_AccelX = 0x01,
    OffScale_AccelY = 0x02,
    OffScale_AccelZ = 0x04,
    OffScale_GyroX  = 0x08,
    OffScale_GyroY  = 0x10,
    OffScale_GyroZ  = 0x20,
  };

  struct ImuSample_t
  {
  double fSampleTime;
  HmdVector3d_t vAccel;
  HmdVector3d_t vGyro;
  uint32_t unOffScaleFlags;
  };
```

The `fSampleTime` is in seconds in the time space of the host system's wall clock. The `unOffScaleFlags` are a mask of values from `enum vr::Imu_OffScaleFlags`, and indicate whether the raw values should be considered off-scale. Some consumers of IMU data may choose to interpret off-scale data differently, or to exclude off-scale axes/orientations.

Fixed factory calibration data for raw IMU data is available via properties on some devices. For example, you may retreive factory calibration data from such a headset as follows:

```cpp
// retrieve the property container of the current HMD
vr::PropertyContainerHandle_t ulContainer = vr::VRProperties()->TrackedDeviceToPropertyContainer( k_unTrackedDeviceIndex_Hmd );

HmdVector3d_t vGyroBias, vGyroScale, vAccelBias, vAccelScale;

// retrieve and save the factory calibration data
vr::VRProperties()->GetProperty( ulContainer, vr::Prop_ImuFactoryGyroBias_Vector3, &vGyroBias, sizeof( HmdVector3_t ), nullptr );
vr::VRProperties()->GetProperty( ulContainer, vr::Prop_ImuFactoryGyroScale_Vector3, &vGyroScale, sizeof( HmdVector3_t ), nullptr );
vr::VRProperties()->GetProperty( ulContainer, vr::Prop_ImuFactoryAccelerometerBias_Vector3, &vAccelBias, sizeof( HmdVector3_t ), nullptr );
vr::VRProperties()->GetProperty( ulContainer, vr::Prop_ImuFactoryAccelerometerScale_Vector3, &vAccelScale, sizeof( HmdVector3_t ), nullptr );
```

To apply fixed factory calibration to a raw sample to get a calibrated sample, perform the following maths:

```cpp

// previously fetched fixed factory calibration data
HmdVector3d_t vGyroBias, vGyroScale, vAccelBias, vAccelScale;

vr::ImuSample_t CalibrateSample( vr::ImuSample_t imuIn )
{
  vr::ImuSample_t imuOut = { .fSampleTime = imuIn.fSampletime, .unOffScaleFlags = imuIn.unOffScaleFlags };
  
  imuOut.vGyro.v[0] = (imuIn.vGyro.v[0] - vGyroBias.v[0]) * vGyroScale.v[0];
  imuOut.vGyro.v[1] = (imuIn.vGyro.v[1] - vGyroBias.v[1]) * vGyroScale.v[1];
  imuOut.vGyro.v[2] = (imuIn.vGyro.v[2] - vGyroBias.v[2]) * vGyroScale.v[2];
  imuOut.vAccel.v[0] = (imuIn.vAccel.v[0] - vAccelBias.v[0]) * vAccelScale.v[0];
  imuOut.vAccel.v[1] = (imuIn.vAccel.v[1] - vAccelBias.v[1]) * vAccelScale.v[1];
  imuOut.vAccel.v[2] = (imuIn.vAccel.v[2] - vAccelBias.v[2]) * vAccelScale.v[2];
  
  return imuOut;
}
```
