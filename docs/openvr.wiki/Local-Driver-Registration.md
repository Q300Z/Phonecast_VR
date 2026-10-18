Drivers need to be added to openvr's configuration in order for them to be located at runtime. The openvr registry file (not to be confused with the Windows registry) is maintained by the utility vrpathreg.exe, which is shipped in the SteamVR directory. SteamVR's install directory can be located on Windows using the registry. On Windows 10, use the standard uninstall key: 

`HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\Steam App 250820`

Vrpathreg.exe makes driver registration tasks easier by handling driver addition, removal, and querying. The most up-to-date information on vrpathreg's commands and usage is available by using the "help" command. The examples below are accurate as of 3/29/2022, but be sure to verify the examples against your current usage information.

Vrpathreg.exe should be used both during testing and development of drivers, and also in the final installer package used to add drivers to an end-user's system.

## Issues to Watch Out For:

* Registering the same driver more than once results in undefined behavior. For example, if an old version of a driver and a new version of a driver are installed to different locations on a system, there is no guarantee which one will be used at runtime. This can lead to problems where one of the drivers is being automatically updated (for example, by Steam), but the other driver is the one actually used at runtime.

* To avoid double-registration issues, be sure to check if your driver is already registered when your installer runs. Always use the `finddriver` command to query for your driver (and other known conflicting drivers) before calling `adddriver`.

* Also be sure to un-register your driver with `removedriver` when uninstalling.

* `removedriverswithname` will remove all drivers with the given name, and should generally be saved for manual troubleshooting.

* Make sure that your installer invokes vrpathreg.exe with the correct elevation level, so that the registry file is not write protected. The local user account should be able to modify the file freely. On Windows, the registry file is in `<user appdata local>/openvr/`. On Linux, the file path is in the path named in env var `HOME` or `~/.config`.