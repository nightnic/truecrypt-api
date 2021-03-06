/* Legal Notice: Portions of the source code contained in this file were 
derived from the source code of TrueCrypt 7.1a which is Copyright (c) 2003-2013 
TrueCrypt Developers Association and is governed by the TrueCrypt License 3.0. 
Modifications and additions to the original source code (contained in this file) 
and all other portions of this file are Copyright (c) 2013 Nic Nilov and are 
governed by license terms which are TBD. */

#include <windows.h>
#include <ShlObj.h>
#include <io.h>
#include "Options.h"
#include "Errors.h"
#include "Apidrvr.h"
#include "Xml.h"
#include "Mount.h"
#include "BootEncryption.h"
#include "Registry.h"
#include "OsInfo.h"

BOOL bPreserveTimestamp = TRUE;
BOOL bCacheInDriver = FALSE;
BOOL bMountReadOnly = FALSE;
BOOL bMountRemovable = FALSE;
BOOL bWipeCacheOnExit = FALSE;		/* Wipe password from cache on exit */

/* NN: Path to TrueCrypt driver. If NULL, denotes use of installed driver, otherwise the one at path. 
Since we load the specified driver only and do not attempt to discover other options, the value of this 
variable defines whether we are working in portable or installed mode. */

char *lpszDriverPath = NULL;

/* This value may changed only by calling ChangeSystemEncryptionStatus(). Only the wizard can change it
(others may still read it though). */
int SystemEncryptionStatus = SYSENC_STATUS_NONE;	

BOOL bPortableModeConfirmed = FALSE;		// TRUE if it is certain that the instance is running in portable mode

BOOL bInPlaceEncNonSysPending = FALSE;		/* TRUE if the non-system in-place encryption config file indicates that one or more partitions are scheduled to be encrypted. */

/* Only the wizard can change this value (others may only read it). */
WipeAlgorithmId nWipeMode = TC_WIPE_NONE;

//TODO: Doc -> options should be freed by caller.
BOOL ApplyOptions(PTCAPI_OPTIONS options) {
	int i;
	DWORD pathSize = 0;
	PTCAPI_OPTION option = NULL;

	defaultMountOptions.Removable =	FALSE;
	defaultMountOptions.ReadOnly =	FALSE;
	defaultMountOptions.ProtectHiddenVolume = FALSE;
	defaultMountOptions.PartitionInInactiveSysEncScope = FALSE;
	defaultMountOptions.RecoveryMode = FALSE;
	defaultMountOptions.UseBackupHeader =  FALSE;

	for (i = 0; i < (int) options->NumberOfOptions; i++) {
		
		option = &options->Options[i];

		switch (option->OptionId) {
		case TC_OPTION_CACHE_PASSWORDS: 
			bCacheInDriver = option->OptionValue;
			break;
		case TC_OPTION_MOUNT_READONLY:
			defaultMountOptions.ReadOnly = bMountReadOnly = option->OptionValue;
			break;
		case TC_OPTION_MOUNT_REMOVABLE:
			defaultMountOptions.Removable = bMountRemovable = option->OptionValue;
			break;
		case TC_OPTION_PRESERVE_TIMESTAMPS:
			bPreserveTimestamp = option->OptionValue;
			break;
		case TC_OPTION_WIPE_CACHE_ON_EXIT:
			bPreserveTimestamp = option->OptionValue;
			break;
		case TC_OPTION_DRIVER_PATH:
			if (option->OptionValue != 0) {
				pathSize = (MAX_PATH + 1);
				lpszDriverPath = (char *) malloc(pathSize);
				memset(lpszDriverPath, 0, (pathSize));
				strcpy(lpszDriverPath, (const char *) (option->OptionValue));
			} else {
				lpszDriverPath = NULL;
			}
			break;
		default:
			set_error_debug_out(TCAPI_E_WRONG_OPTION);
			return FALSE;
		}
	}

	if (!LoadStoredSettings()) {
		//TODO: Doc -> See GetLastError()
		return FALSE;
	}
	return TRUE;
}

static BOOL LoadStoredSettings() {
	WipeAlgorithmId savedWipeAlgorithm = TC_WIPE_NONE;

	EnableHwEncryption ((ReadDriverConfigurationFlags() & TC_DRIVER_CONFIG_DISABLE_HARDWARE_ENCRYPTION) ? FALSE : TRUE);

	if (TryDetectSystemEncryptionStatus()) {
		set_error_debug_out(TCAPI_E_TC_CONFIG_CORRUPTED);
		return FALSE;
	}

	//TODO: need to check if this is an issue for us.
	if (TryDetectNonSysInPlaceEncSettings (&savedWipeAlgorithm) != 0)
		bInPlaceEncNonSysPending = TRUE;

	mountOptions = defaultMountOptions;

	//TODO: Boot project transfer
	//if (IsHiddenOSRunning())
	//	HiddenSysLeakProtectionNotificationStatus =	ConfigReadInt ("HiddenSystemLeakProtNotifStatus", TC_HIDDEN_OS_READ_ONLY_NOTIF_MODE_NONE);

	return TRUE;
}

char *GetModPath (char *path, int maxSize)
{
	/* NN: Instead of GetModuleFileName() we check path 
	   to executable in address space of which we reside. */
	strrchr (_pgmptr, '\\')[1] = 0;
	return path;
}

BOOL IsNonInstallMode() {
	return (lpszDriverPath != NULL);
}

char *GetConfigPath (char *fileName)
{
	static char path[MAX_PATH * 2] = { 0 };

	if (IsNonInstallMode ())
	{
		GetModPath (path, sizeof (path));
		strcat (path, fileName);

		return path;
	}

	if (SUCCEEDED(SHGetFolderPath (NULL, CSIDL_APPDATA | CSIDL_FLAG_CREATE, NULL, 0, path)))
	{
		strcat (path, "\\TrueCrypt\\");
		CreateDirectory (path, NULL);
		strcat (path, fileName);
	}
	else
		path[0] = 0;

	return path;
}

// Returns NULL if there's any error. Although the buffer can contain binary data, it is always null-terminated.
char *LoadFile (const char *fileName, DWORD *size)
{
	char *buf;
	HANDLE h = CreateFile (fileName, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
	if (h == INVALID_HANDLE_VALUE)
		return NULL;

	*size = GetFileSize (h, NULL);
	buf = (char *) malloc (*size + 1);

	if (buf == NULL)
	{
		CloseHandle (h);
		return NULL;
	}

	ZeroMemory (buf, *size + 1);

	if (!ReadFile (h, buf, *size, size, NULL))
	{
		free (buf);
		buf = NULL;
	}

	CloseHandle (h);
	return buf;
}

// nee BOOL LoadSysEncSettings (void)
BOOL TryDetectSystemEncryptionStatus (void)
{
	DWORD size = 0;
	char *sysEncCfgFileBuf = NULL;
	char *xml = NULL;
	char *configPath = NULL;
	char paramName[100], paramVal[MAX_PATH];

	// Defaults
	int newSystemEncryptionStatus = SYSENC_STATUS_NONE;
	WipeAlgorithmId newnWipeMode = TC_WIPE_NONE;

	/* TODO: In case portable TrueCrypt was used to set up system encryption and has not finished
	   we won't be able to detect the state by config files since we don't know the TC's directory.
	   Actually it seems TC itself when used from another folder will not be able to do that.
	   Will have to check whether boot-related facilities can tell more */

	configPath = GetConfigPath (TC_APPD_FILENAME_SYSTEM_ENCRYPTION);
	if (!FileExists (configPath))
	{
		SystemEncryptionStatus = newSystemEncryptionStatus;
		nWipeMode = newnWipeMode;
	} 
	else {
		sysEncCfgFileBuf = LoadFile (configPath, &size);
		xml = sysEncCfgFileBuf;
	}

	if (xml == NULL)
	{
		return FALSE;
	}

	while (xml = XmlFindElement (xml, "config"))
	{
		XmlGetAttributeText (xml, "key", paramName, sizeof (paramName));
		XmlGetNodeText (xml, paramVal, sizeof (paramVal));

		if (strcmp (paramName, "SystemEncryptionStatus") == 0)
		{
			newSystemEncryptionStatus = atoi (paramVal);
		}
		else if (strcmp (paramName, "WipeMode") == 0)
		{
			newnWipeMode = (WipeAlgorithmId) atoi (paramVal);
		}

		xml++;
	}

	SystemEncryptionStatus = newSystemEncryptionStatus;
	nWipeMode = newnWipeMode;

	free (sysEncCfgFileBuf);
	return TRUE;
}

// Returns the number of partitions where non-system in-place encryption is progress or had been in progress
// but was interrupted. In addition, via the passed pointer, returns the last selected wipe algorithm ID.
// nee int LoadNonSysInPlaceEncSettings (WipeAlgorithmId *wipeAlgorithm)
int TryDetectNonSysInPlaceEncSettings (WipeAlgorithmId *wipeAlgorithm)
{
	char *fileBuf = NULL;
	char *fileBuf2 = NULL;
	DWORD size, size2;
	int count;

	*wipeAlgorithm = TC_WIPE_NONE;

	if (!FileExists (GetConfigPath (TC_APPD_FILENAME_NONSYS_INPLACE_ENC)))
		return 0;

	if ((fileBuf = LoadFile (GetConfigPath (TC_APPD_FILENAME_NONSYS_INPLACE_ENC), &size)) == NULL)
		return 0;

	if (FileExists (GetConfigPath (TC_APPD_FILENAME_NONSYS_INPLACE_ENC_WIPE)))
	{
		if ((fileBuf2 = LoadFile (GetConfigPath (TC_APPD_FILENAME_NONSYS_INPLACE_ENC_WIPE), &size2)) != NULL)
			*wipeAlgorithm = (WipeAlgorithmId) atoi (fileBuf2);
	}

	count = atoi (fileBuf);

	if (fileBuf != NULL)
		TCfree (fileBuf);

	if (fileBuf2 != NULL)
		TCfree (fileBuf2);

	return (count);
}

uint32 ReadEncryptionThreadPoolFreeCpuCountLimit ()
{
	DWORD count;

	if (!ReadLocalMachineRegistryDword ("SYSTEM\\CurrentControlSet\\Services\\truecrypt", TC_ENCRYPTION_FREE_CPU_COUNT_REG_VALUE_NAME, &count))
		count = 0;

	return count;
}