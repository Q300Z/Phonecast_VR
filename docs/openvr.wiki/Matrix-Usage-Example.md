Matrices and axes are the source of some confusion in 3d programming because it's not obvious if a matrix is column-major or row-major without reading the source, and because 3D game engines and 3D editing applications do not consistently use the same axes for the same things. For example, the Unity engine uses +Z forward, but the Unreal engine uses +X forward, and SteamVR uses -Z forward.

OpenVR uses "B from A" matrices in some places, instead of "A to B" matrices. The excellent Tom Forsyth has a post explaining the convenience of this convention on his blog: [http://eelpi.gotdns.org/](http://eelpi.gotdns.org/), please search for the article titled `Matrix maths and names`.

In OpenVR, HmdMatrix34_t is used to represent a rigid transform (rotation plus translation).

This can be written with row-major storage as:

```
m[0][0] m[0][1] m[0][2] m[0][3]

m[1][0] m[1][1] m[1][2] m[1][3]

m[2][0] m[2][1] m[2][2] m[2][3]
```

This is stored in memory as:

`m[0][0], m[0][1], m[0][2], m[0][3], m[1][0], m[1][1], m[1][2], m[1][3], m[2][0], ...`

Given the three axes AX, AY, AZ, and translation T, we would represent that in the above layout as:

```
AXx AYx AZx Tx

AXy AYy AZy Ty

AXz AYz AZz Tz
```

Note this implies these use column-major mathematical notation.  The axes and translation vectors are represented as column vectors, while their memory layout is row-major.

This is important because column-major notation uses Post-Multiplication (Mv = matrix times vector) rather than Pre-Multiplication (vM = vector times matrix).

## Examining Hellovr

We recommend making use of the [OpenVR Samples](https://github.com/ValveSoftware/openvr/tree/master/samples) in this github repo, particularly the 'hellovr' samples. These are simple programs that are easy to study, run in a debugger, and experiment with. As you work on your driver, it may help to strip out complexity from these 'hellovr' programs and slowly add it back in as your driver becomes more complete.

Note that Hellovr converts from OpenVR's vr::HmdMatrix34_t to the Matrix4 type.

Let's walk through the data feeds into GetCurrentViewProjectionMatrix, as this function is called in hellovr's RenderScene function.

```
Matrix4 CMainApplication::GetCurrentViewProjectionMatrix( vr::Hmd_Eye nEye )
{
	matMVP = m_mat4ProjectionLeft * m_mat4eyePosLeft * m_mat4HMDPose
	return matMVP;
}

void CMainApplication::RenderScene( vr::Hmd_Eye nEye )
{
	// Update the persistently mapped pointer to the CB data with the latest matrix
	memcpy( m_pSceneConstantBufferData[ nEye ], GetCurrentViewProjectionMatrix( nEye ).get(), sizeof( Matrix4 ) );
...
```

Here are parts of the functions that provide the necessary data above, but code has been omitted for clarity:

```
void CMainApplication::SetupCameras()
{
	m_mat4ProjectionLeft = GetHMDMatrixProjectionEye( vr::Eye_Left );
	m_mat4ProjectionRight = GetHMDMatrixProjectionEye( vr::Eye_Right );
	m_mat4eyePosLeft = GetHMDMatrixPoseEye( vr::Eye_Left );
	m_mat4eyePosRight = GetHMDMatrixPoseEye( vr::Eye_Right );
}

Matrix4 CMainApplication::GetHMDMatrixProjectionEye( vr::Hmd_Eye nEye )
{
	//m_pHMD is of type vr::IVRSystem*
	vr::HmdMatrix44_t mat = m_pHMD->GetProjectionMatrix( nEye, m_fNearClip, m_fFarClip );

	// The vr::HmdMatrix44_t is transposed to switch the row/column order, but the matrix is not inverted.
	return Matrix4(
		mat.m[0][0], mat.m[1][0], mat.m[2][0], mat.m[3][0],
		mat.m[0][1], mat.m[1][1], mat.m[2][1], mat.m[3][1], 
		mat.m[0][2], mat.m[1][2], mat.m[2][2], mat.m[3][2], 
		mat.m[0][3], mat.m[1][3], mat.m[2][3], mat.m[3][3]
	);
}

Matrix4 CMainApplication::GetHMDMatrixPoseEye( vr::Hmd_Eye nEye )
{
	vr::HmdMatrix34_t matEyeRight = m_pHMD->GetEyeToHeadTransform( nEye );

	//Note that this matrix is transposed and inverted
	Matrix4 matrixObj(
		matEyeRight.m[0][0], matEyeRight.m[1][0], matEyeRight.m[2][0], 0.0, 
		matEyeRight.m[0][1], matEyeRight.m[1][1], matEyeRight.m[2][1], 0.0,
		matEyeRight.m[0][2], matEyeRight.m[1][2], matEyeRight.m[2][2], 0.0,
		matEyeRight.m[0][3], matEyeRight.m[1][3], matEyeRight.m[2][3], 1.0f
		);

	return matrixObj.invert();
}

void CMainApplication::UpdateHMDMatrixPose()
{

	vr::VRCompositor()->WaitGetPoses(m_rTrackedDevicePose, vr::k_unMaxTrackedDeviceCount, NULL, 0 );
	... //Iterating over nDevice
	{
		//ConvertSteamVRMatrixToMatrix4 will transpose the matrix
		m_rmat4DevicePose[nDevice] = ConvertSteamVRMatrixToMatrix4( m_rTrackedDevicePose[nDevice].mDeviceToAbsoluteTracking );
	}
	m_mat4HMDPose = m_rmat4DevicePose[vr::k_unTrackedDeviceIndex_Hmd];
	m_mat4HMDPose.invert();
}

```

The operations for one eye can be summed up in this pseudo-code:
```
m_mat4Projection = Transpose( m_pHMD->GetProjectionMatrix( ... ) )
m_mat4eyePos = Invert( Transpose( m_pHMD->GetEyeToHeadTransform( ... ) ) )
m_mat4HMDPose = Invert( Transpose( HmdPose.mDeviceToAbsoluteTracking ) )
matMVP = m_mat4ProjectionLeft * m_mat4eyePosLeft * m_mat4HMDPose
```