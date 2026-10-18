Every driver dynamic library needs to implement the standard driver factory function and use it to return implementations of the OpenVR driver interfaces. This function is the entry-point for every driver.

A typical factory function looks like this:

```cpp
#define HMD_DLL_EXPORT extern "C" __declspec( dllexport )

HMD_DLL_EXPORT 
void *HmdDriverFactory( const char *pInterfaceName, int *pReturnCode )
{
	if( 0 == strcmp( IServerTrackedDeviceProvider_Version, pInterfaceName ) )
	{
		return <global for server driver provider>;
	}
	if( 0 == strcmp( IVRWatchdogProvider_Version, pInterfaceName ) )
	{
		return <global for watchdog driver>;
	}


	if( pReturnCode )
		*pReturnCode = HmdError_Init_InterfaceNotFound;

	return NULL;
}
```

If you wish to implement [IVRWatchdogProvider](IVRWatchdogProvider_Overview), please see the example code in the sample driver.