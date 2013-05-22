// Api.c : Defines the exported functions for the DLL application.
//

#include "Api.h"
#include "Errors.h"
#include "OsInfo.h"
#include "EncryptionThreadPool.h"
#include "Apidrvr.h"
#include "Ipc.h"

BOOL bTcApiInitialized = FALSE;

#define TCAPI_CHECK_INITIALIZED(RESULT) do { if (!bTcApiInitialized) { SetLastError(TCAPI_E_NOT_INITIALIZED); return RESULT; } } while (0)

DLLEXPORT BOOL APIENTRY Initialize(PTCAPI_OPTIONS options) {

	if (!InitOSVersionInfo()) {
		//TODO: Doc -> See GetLastError()
		//TODO: Doc -> Warnings may be saturated by subsequent errors
		//TODO: Save warnings here and pass at the end in case of no error
		return FALSE;
	}

	if (IsTrueCryptInstallerRunning()) {
		debug_out("TCAPI_E_TC_INSTALLER_RUNNING", TCAPI_E_TC_INSTALLER_RUNNING);
		SetLastError(TCAPI_E_TC_INSTALLER_RUNNING);
		return FALSE;
	}

	if (!options || !ApplyOptions(options)) {
		//TODO: Doc -> See GetLastError()
		return FALSE;
	}

	if (!EncryptionThreadPoolStart (ReadEncryptionThreadPoolFreeCpuCountLimit()))
	{
		//TODO: Doc -> See GetLastError()
		debug_out("TCAPI_E_CANT_START_ENCPOOL", TCAPI_E_CANT_START_ENCPOOL);
		SetLastError(TCAPI_E_CANT_START_ENCPOOL);
		return FALSE;
	}

	bTcApiInitialized = TRUE;
	return bTcApiInitialized;
}

DLLEXPORT BOOL APIENTRY Shutdown(void) {

	//returns FALSE if not initialized
	TCAPI_CHECK_INITIALIZED(0);

	EncryptionThreadPoolStop();
	return TRUE;
}

DLLEXPORT BOOL APIENTRY LoadTrueCryptDriver(void)
{
	int status = FALSE;
	
	TCAPI_CHECK_INITIALIZED(0);
	
	status = DriverAttach ();
	if (status == FALSE)
	{
		//TODO: Doc -> See GetLastError()
	}

	return status;
}

DLLEXPORT BOOL APIENTRY UnloadTrueCryptDriver(void)
{
	BOOL status = FALSE;

	TCAPI_CHECK_INITIALIZED(0);

	status = DriverUnload ();
	if (status == FALSE)
	{
		//TODO: Doc -> See GetLastError()
	}

	return status;
}
