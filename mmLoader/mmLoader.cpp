/********************************************************************
created:	2014/03/11
created:	11:3:2014   14:39
file base:	mmLoader
file ext:	cpp
author:		tishion

purpose:
*********************************************************************/
#include <windows.h>
#include "mmLoader.h"
//////////////////////////////////////////////////////////////////////////
// declaration
//
EXTERN_C
{
	BOOL OpenAndMapView(LPCTSTR pFilePathName, PMEM_MODULE pMemModule);
	BOOL IsValidPEFormat(LPVOID pBuffer);
	BOOL MapMemModuleSections(PMEM_MODULE pMemModule);
	BOOL ReleaseRawFileResource(PMEM_MODULE pMemModule);
	BOOL RelocateMemModule(PMEM_MODULE pMemModule);
	BOOL ResolveImports(PMEM_MODULE pMemModule);
	BOOL SetMemProtectStatus(PMEM_MODULE pMemModule);
	BOOL CallModuleEntry(PMEM_MODULE pMemModule, DWORD dwReason);
	FARPROC GetExportedProcAddress(PMEM_MODULE pMemModule, LPCSTR lpName);
	VOID UnmapMemModule(PMEM_MODULE pMemModule);
};

#include "strmem.h"
#include "crc.h"

//////////////////////////////////////////////////////////////////////////
// implementation
// 

VOID _declspec(naked) mmLoaderSCStart()
{
	__asm jmp MemModuleHelper;
	_asm _emit 'm';
	_asm _emit 'm';
	_asm _emit 'L';
	_asm _emit 'o';
	_asm _emit 'a';
	_asm _emit 'd';
	_asm _emit 'e';
	_asm _emit 'r';
	_asm _emit 'S';
	_asm _emit 'C';
	_asm _emit 'S';
	_asm _emit 't';
	_asm _emit 'a';
	_asm _emit 'r';
	_asm _emit 't';
}

int __stdcall MemModuleHelper(
	PMEM_MODULE pMmeModule,
	MMHELPER_METHOD method,
	LPCTSTR lpModuleName,
	LPCSTR lpProcName,
	BOOL bCallEntry)
{
	switch (method)
	{
	case MHM_BOOL_LOAD:
	{
		return (int)LoadMemModule(pMmeModule, lpModuleName, bCallEntry);
	}
	break;
	case MHM_VOID_FREE:
	{
		FreeMemModule(pMmeModule);
	}
	break;
	case MHM_FARPROC_GETPROC:
	{
		return (int)GetMemModuleProc(pMmeModule, lpProcName);
	}
	break;
	default:
		break;
	}

	return 0;
}

BOOL __stdcall LoadMemModule(PMEM_MODULE pMemModule, LPCTSTR lpName, BOOL bCallEntry)
{
	if (NULL == pMemModule
		|| NULL == pMemModule->pNtFuncptrsTable
		|| NULL == lpName)
	{
		return FALSE;
	}

	Dw_memset(pMemModule, 0, sizeof(pMemModule));
	pMemModule->RawFile.h = INVALID_HANDLE_VALUE;

	// open file, map memory
	if (FALSE == OpenAndMapView(lpName, pMemModule))
	{
		return FALSE;
	}

	// verify file format
	if (FALSE == IsValidPEFormat(pMemModule->RawFile.pBuffer))
	{
		return FALSE;
	}

	// map PE header and section table into memory
	if (FALSE == MapMemModuleSections(pMemModule))
	{
		return FALSE;
	}

	if (FALSE == ReleaseRawFileResource(pMemModule))
	{
		return FALSE;
	}

	// relocation, resolving imports table
	if (FALSE == RelocateMemModule(pMemModule)
		|| FALSE == ResolveImports(pMemModule))
	{
		UnmapMemModule(pMemModule);
		return FALSE;
	}

	if (FALSE == SetMemProtectStatus(pMemModule))
	{
		UnmapMemModule(pMemModule);
		return FALSE;
	}

	if (bCallEntry)
	{
		if (FALSE == CallModuleEntry(pMemModule, DLL_PROCESS_ATTACH))
		{
			// failed to call entry point,
			// clean resource, return false
			UnmapMemModule(pMemModule);
			return FALSE;
		}
	}

	pMemModule->dwCrc = GetCrc32(
		0, pMemModule->lpBase, pMemModule->dwSizeOfImage);

	Dw_strcpyW(pMemModule->tszModuleName, lpName, _countof(pMemModule->tszModuleName));

	return TRUE;
}

VOID __stdcall FreeMemModule(PMEM_MODULE pMemModule)
{
	if (NULL != pMemModule)
	{
		CallModuleEntry(pMemModule, DLL_PROCESS_DETACH);
		UnmapMemModule(pMemModule);
		//__try
		//{
		//	CallModuleEntry(pMemModule, DLL_PROCESS_DETACH);
		//}
		//__finally
		//{
		//	UnmapMemModule(pMemModule);
		//}
	}
}

FARPROC __stdcall GetMemModuleProc(PMEM_MODULE pMemModule, LPCSTR lpName)
{
	if (NULL != pMemModule
		&& lpName != NULL)
	{
		return GetExportedProcAddress(pMemModule, lpName);
	}

	return NULL;
}


//////////////////////////////////////////////////////////////////////////
// useful macros
// 
#define IfFalseGoExit(x) { br=(x); if (!br) goto _Exit; }
#define MakePointer(t, p, offset) ((t)((ULONG)(p) + offset))

//////////////////////////////////////////////////////////////////////////
// FileUtils
//
BOOL OpenAndMapView(LPCTSTR pFilePathName, PMEM_MODULE pMemModule)
{
	if (NULL == pMemModule
		|| NULL == pMemModule->pNtFuncptrsTable)
	{
		return FALSE;
	}

	typedef HANDLE(WINAPI * Type_CreateFileW)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
	typedef DWORD(WINAPI * Type_GetFileSize)(HANDLE, LPDWORD);
	typedef HANDLE(WINAPI * Type_CreateFileMappingW)(HANDLE, LPSECURITY_ATTRIBUTES, DWORD, DWORD, DWORD, LPCWSTR);
	typedef LPVOID(WINAPI * Type_MapViewOfFile)(HANDLE, DWORD, DWORD, DWORD, SIZE_T);

	Type_CreateFileW pfnCreateFileW = (Type_CreateFileW)(pMemModule->pNtFuncptrsTable->pfnCreateFileW);
	Type_GetFileSize pfnGetFileSize = (Type_GetFileSize)(pMemModule->pNtFuncptrsTable->pfnGetFileSize);
	Type_CreateFileMappingW pfnCreateFileMappingW = (Type_CreateFileMappingW)(pMemModule->pNtFuncptrsTable->pfnCreateFileMappingW);
	Type_MapViewOfFile pfnMapViewOfFile = (Type_MapViewOfFile)(pMemModule->pNtFuncptrsTable->pfnMapViewOfFile);

	BOOL br = FALSE;
	pMemModule->RawFile.h = pfnCreateFileW(
		pFilePathName, GENERIC_READ, FILE_SHARE_READ,
		NULL, OPEN_EXISTING, NULL, NULL);
	IfFalseGoExit(INVALID_HANDLE_VALUE != pMemModule->RawFile.h);
	IfFalseGoExit(NULL != pMemModule->RawFile.h);

	// check file size
	DWORD dwFileSize = pfnGetFileSize(pMemModule->RawFile.h, NULL);
	IfFalseGoExit(INVALID_FILE_SIZE != dwFileSize);

	// if file size is less than DOS header, invalid file.
	IfFalseGoExit(dwFileSize > (sizeof(IMAGE_DOS_HEADER)));

	pMemModule->RawFile.hMapping = pfnCreateFileMappingW(
		pMemModule->RawFile.h, 0, PAGE_READONLY, 0, 0, NULL);
	IfFalseGoExit(NULL != pMemModule->RawFile.hMapping);

	pMemModule->RawFile.pBuffer = pfnMapViewOfFile(
		pMemModule->RawFile.hMapping, FILE_MAP_READ, 0, 0, 0);
	IfFalseGoExit(NULL != pMemModule->RawFile.pBuffer);

_Exit:
	return br;
}

//////////////////////////////////////////////////////////////////////////
// close file mapping, file, release resource
// 
BOOL ReleaseRawFileResource(PMEM_MODULE pMemModule)
{
	if (NULL == pMemModule
		|| NULL == pMemModule->pNtFuncptrsTable)
	{
		return FALSE;
	}

	typedef BOOL(WINAPI * Type_UnmapViewOfFile)(LPVOID);
	typedef BOOL(WINAPI * Type_CloseHandle)(HANDLE);

	Type_UnmapViewOfFile pfnUnmapViewOfFile = (Type_UnmapViewOfFile)(pMemModule->pNtFuncptrsTable->pfnUnmapViewOfFile);
	Type_CloseHandle pfnCloseHandle = (Type_CloseHandle)(pMemModule->pNtFuncptrsTable->pfnCloseHandle);

	if (pMemModule->RawFile.pBuffer)
	{
		pfnUnmapViewOfFile(pMemModule->RawFile.pBuffer);
		pMemModule->RawFile.pBuffer = NULL;
	}

	if (pMemModule->RawFile.hMapping)
	{
		pfnCloseHandle(pMemModule->RawFile.hMapping);
		pMemModule->RawFile.hMapping = NULL;
	}

	if (pMemModule->RawFile.h)
	{
		pfnCloseHandle(pMemModule->RawFile.h);
		pMemModule->RawFile.h = INVALID_HANDLE_VALUE;
	}

	return TRUE;
}

//////////////////////////////////////////////////////////////////////////
// PeUtils
// 

/*
  * Verify the buffer fro valid PE file
 *
 */
BOOL IsValidPEFormat(LPVOID pBuffer)
{
	if (NULL == pBuffer)
	{
		return FALSE;
	}

	BOOL br = FALSE;

	PIMAGE_DOS_HEADER pImageDosHeader = (PIMAGE_DOS_HEADER)pBuffer;

	// check the MZ signature
	IfFalseGoExit(IMAGE_DOS_SIGNATURE == pImageDosHeader->e_magic);

	// check PE signature
	DWORD dwE_lfanew = pImageDosHeader->e_lfanew;
	PDWORD pdwPESignature = MakePointer(PDWORD, pBuffer, dwE_lfanew);
	IfFalseGoExit(IMAGE_NT_SIGNATURE == *pdwPESignature);

	// get IMAGE_FILE_HEADER, and check the target platform and CPU architecture
	PIMAGE_FILE_HEADER pImageFileHeader =
		MakePointer(PIMAGE_FILE_HEADER, pdwPESignature, sizeof(IMAGE_NT_SIGNATURE));

	IfFalseGoExit(IMAGE_FILE_MACHINE_I386 == pImageFileHeader->Machine);

	PIMAGE_NT_HEADERS32 pImageNtHeader32 =
		MakePointer(PIMAGE_NT_HEADERS32, pdwPESignature, 0);

	IfFalseGoExit(
		IMAGE_NT_OPTIONAL_HDR32_MAGIC == pImageNtHeader32->OptionalHeader.Magic);

	if (IMAGE_FILE_MACHINE_I386 == pImageFileHeader->Machine)
	{
		// maybe PE32, go on to verify the magic in OptionalHeader
		PIMAGE_NT_HEADERS32 pImageNtHeader32 =
			MakePointer(PIMAGE_NT_HEADERS32, pdwPESignature, 0);

		IfFalseGoExit(
			IMAGE_NT_OPTIONAL_HDR32_MAGIC == pImageNtHeader32->OptionalHeader.Magic);
		// it is sure this is 32 bit module
	}
	//else if(IMAGE_FILE_MACHINE_AMD64 == pImageFileHeader->Machine)
	//{
	//	// maybe PE64, go on to verify the magic in OptionalHeader
	//	PIMAGE_NT_HEADERS64 pImageNtHeader64 = 
	//		MakePointer(PIMAGE_NT_HEADERS64, pdwPESignature, 0);

	//	IfFalseGoExit(
	//		IMAGE_NT_OPTIONAL_HDR64_MAGIC == pImageNtHeader64->OptionalHeader.Magic);
	//	// it is sure this is PE64
	//	// only support 32 bit module for now
	//	IfFalseGoExit(FALSE);
	//}
	else
	{
		// unsupported format
		IfFalseGoExit(FALSE);
	}

_Exit:
	return br;
}

/*
 * get alignment size
 */
#define AlignmentSize(s, a)  (((s + a - 1) / a) * a )


/*
 * map PE Header and all sections.
 */
BOOL MapMemModuleSections(PMEM_MODULE pMemModule)
{
	if (NULL == pMemModule
		|| NULL == pMemModule->pNtFuncptrsTable
		|| NULL == pMemModule->RawFile.pBuffer)
	{
		return FALSE;
	}

	typedef LPVOID(WINAPI * Type_VirtualAlloc)(LPVOID, SIZE_T, DWORD, DWORD);
	typedef BOOL(WINAPI * Type_VirtualFree)(LPVOID, SIZE_T, DWORD);

	Type_VirtualAlloc pfnVirtualAlloc = (Type_VirtualAlloc)(pMemModule->pNtFuncptrsTable->pfnVirtualAlloc);
	Type_VirtualFree pfnVirtualFree = (Type_VirtualFree)(pMemModule->pNtFuncptrsTable->pfnVirtualFree);

	PIMAGE_DOS_HEADER pImageDosHeader = (PIMAGE_DOS_HEADER)(pMemModule->RawFile.pBuffer);

	PIMAGE_NT_HEADERS pImageNtHeader = MakePointer(
		PIMAGE_NT_HEADERS32, pImageDosHeader, pImageDosHeader->e_lfanew);

	// reserve virtual memory
	LPVOID lpBase = pfnVirtualAlloc(
		(LPVOID)(pImageNtHeader->OptionalHeader.ImageBase),
		pImageNtHeader->OptionalHeader.SizeOfImage,
		MEM_RESERVE | MEM_COMMIT,
		PAGE_READWRITE);

	// can't reserve space at ImageBase, then it's up to the system
	if (NULL == lpBase)
	{
		lpBase = pfnVirtualAlloc(
			NULL,
			pImageNtHeader->OptionalHeader.SizeOfImage,
			MEM_RESERVE | MEM_COMMIT,
			PAGE_READWRITE);

		if (NULL == lpBase)
		{
			// still failed, there may not be sufficient memory
			return FALSE;
		}
	}

	// copy PE header to target address
	Dw_memmove(
		lpBase,
		pMemModule->RawFile.pBuffer,
		pImageNtHeader->OptionalHeader.SizeOfHeaders);

	int nNumberOfSections = pImageNtHeader->FileHeader.NumberOfSections;
	PIMAGE_SECTION_HEADER pImageSectionHeader = MakePointer(
		PIMAGE_SECTION_HEADER, pImageNtHeader, sizeof(IMAGE_NT_HEADERS32));

	DWORD dwSectionBase = NULL;

	for (int i = 0; i < nNumberOfSections; ++i)
	{
		if (0 != pImageSectionHeader[i].VirtualAddress && 0 != pImageSectionHeader[i].SizeOfRawData)
		{
			dwSectionBase = pImageSectionHeader[i].VirtualAddress + (DWORD)lpBase;

			// copy this section to target address
			Dw_memmove(
				(LPVOID)dwSectionBase,
				(LPVOID)((DWORD)pMemModule->RawFile.pBuffer + pImageSectionHeader[i].PointerToRawData),
				pImageSectionHeader[i].SizeOfRawData);
		}
	}

	// store the base address of this module.
	pMemModule->lpBase = lpBase;
	pMemModule->dwSizeOfImage = pImageNtHeader->OptionalHeader.SizeOfImage;
	pMemModule->bLoadOk = TRUE;

	return TRUE;
}

/*
 * relocation
 */
BOOL RelocateMemModule(PMEM_MODULE pMemModule)
{
	if (NULL == pMemModule
		|| NULL == pMemModule->pImageDosHeader)
	{
		return FALSE;
	}

	PIMAGE_NT_HEADERS32 pImageNtHeader = MakePointer(
		PIMAGE_NT_HEADERS32,
		pMemModule->pImageDosHeader,
		pMemModule->pImageDosHeader->e_lfanew);

	DWORD dwDelta = pMemModule->dwBase - pImageNtHeader->OptionalHeader.ImageBase;

	// this module has been loaded to the ImageBase, no need to do relocation
	if (0 == dwDelta)
	{
		return TRUE;
	}

	if (0 == pImageNtHeader->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].VirtualAddress
		|| 0 == pImageNtHeader->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].Size)
	{
		return TRUE;
	}

	PIMAGE_BASE_RELOCATION pImageBaseRelocation = MakePointer(
		PIMAGE_BASE_RELOCATION,
		pMemModule->lpBase,
		pImageNtHeader->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].VirtualAddress);

	if (NULL == pImageBaseRelocation)
	{
		return FALSE;
	}

	while (0 != (pImageBaseRelocation->VirtualAddress + pImageBaseRelocation->SizeOfBlock))
	{
		PWORD pRelocationData = MakePointer(PWORD, pImageBaseRelocation, sizeof(IMAGE_BASE_RELOCATION));

		int NumberOfRelocationData = (pImageBaseRelocation->SizeOfBlock
			- sizeof(IMAGE_BASE_RELOCATION)) / sizeof(WORD);

		for (int i = 0; i < NumberOfRelocationData; i++)
		{
			if (/*0x00003000*/IMAGE_REL_BASED_HIGHLOW == (pRelocationData[i] >> 12))
			{
				PDWORD pAddress = (PDWORD)(pMemModule->dwBase + pImageBaseRelocation->VirtualAddress + (pRelocationData[i] & 0x0FFF));
				*pAddress += dwDelta;
			}
		}

		pImageBaseRelocation = (PIMAGE_BASE_RELOCATION)((DWORD)pImageBaseRelocation + pImageBaseRelocation->SizeOfBlock);
	}

	return TRUE;
}

/*
 * resolve import table 
 */
BOOL ResolveImports(PMEM_MODULE pMemModule)
{
	if (NULL == pMemModule
		|| NULL == pMemModule->pNtFuncptrsTable
		|| NULL == pMemModule->pImageDosHeader)
	{
		return FALSE;
	}

	typedef HMODULE(WINAPI * Type_GetModuleHandleA)(LPCSTR);
	typedef HMODULE(WINAPI * Type_LoadLibraryA)(LPCSTR);
	typedef FARPROC(WINAPI * Type_GetProcAddress)(HMODULE, LPCSTR);

	Type_GetModuleHandleA pfnGetModuleHandleA = (Type_GetModuleHandleA)(pMemModule->pNtFuncptrsTable->pfnGetModuleHandleA);
	Type_LoadLibraryA pfnLoadLibraryA = (Type_LoadLibraryA)(pMemModule->pNtFuncptrsTable->pfnLoadLibraryA);
	Type_GetProcAddress pfnGetProcAddress = (Type_GetProcAddress)(pMemModule->pNtFuncptrsTable->pfnGetProcAddress);


	PIMAGE_NT_HEADERS32 pImageNtHeader = MakePointer(
		PIMAGE_NT_HEADERS32,
		pMemModule->pImageDosHeader,
		pMemModule->pImageDosHeader->e_lfanew);

	if (pImageNtHeader->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress == 0
		|| pImageNtHeader->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].Size == 0)
	{
		return TRUE;
	}

	PIMAGE_IMPORT_DESCRIPTOR pImageImportDescriptor = MakePointer(
		PIMAGE_IMPORT_DESCRIPTOR,
		pMemModule->lpBase,
		pImageNtHeader->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);

	while (pImageImportDescriptor->OriginalFirstThunk)
	{
		PCHAR pDllName = MakePointer(PCHAR, pMemModule->lpBase, pImageImportDescriptor->Name);
		HMODULE hMod = pfnGetModuleHandleA(pDllName);

		if (NULL == hMod)
		{
			hMod = pfnLoadLibraryA(pDllName);
		}

		if (NULL != hMod)
		{
			DWORD OriginalFirstThunk = pImageImportDescriptor->OriginalFirstThunk;
			DWORD FirstThunk = pImageImportDescriptor->FirstThunk;

			PIMAGE_THUNK_DATA32 pOrgItemEntry = MakePointer(PIMAGE_THUNK_DATA32, pMemModule->lpBase, OriginalFirstThunk);

			PIMAGE_THUNK_DATA32 pIatItemEntry = MakePointer(PIMAGE_THUNK_DATA32, pMemModule->lpBase, FirstThunk);

			while (0 != pOrgItemEntry->u1.AddressOfData)
			{
				FARPROC lpFunction = NULL;
				if (pOrgItemEntry->u1.AddressOfData & IMAGE_ORDINAL_FLAG32)
				{
					lpFunction = pfnGetProcAddress(hMod, (LPCSTR)(IMAGE_ORDINAL32(pOrgItemEntry->u1.Ordinal)));
				}
				else
				{
					PIMAGE_IMPORT_BY_NAME pImageImportByName = MakePointer(
						PIMAGE_IMPORT_BY_NAME, pMemModule->lpBase, pOrgItemEntry->u1.AddressOfData);

					lpFunction = pfnGetProcAddress(hMod, (LPCSTR)(pImageImportByName->Name));
				}

				// write into IAT
				pIatItemEntry->u1.Function = (DWORD)lpFunction;

				pOrgItemEntry = MakePointer(PIMAGE_THUNK_DATA32, pOrgItemEntry, sizeof(DWORD));
				pIatItemEntry = MakePointer(PIMAGE_THUNK_DATA32, pIatItemEntry, sizeof(DWORD));
			}
		}
		else
		{
			//assert(false);
			return FALSE;
		}

		pImageImportDescriptor = MakePointer(
			PIMAGE_IMPORT_DESCRIPTOR,
			pImageImportDescriptor,
			sizeof(IMAGE_IMPORT_DESCRIPTOR));
	}

	return TRUE;
}


BOOL SetMemProtectStatus(PMEM_MODULE pMemModule)
{
	if (NULL == pMemModule
		|| NULL == pMemModule->pNtFuncptrsTable)
	{
		return FALSE;
	}

	typedef BOOL(WINAPI * Type_VirtualProtect)(LPVOID, SIZE_T, DWORD, PDWORD);

	Type_VirtualProtect pfnVirtualProtect = (Type_VirtualProtect)(pMemModule->pNtFuncptrsTable->pfnVirtualProtect);

	BOOL br = FALSE;

	PIMAGE_DOS_HEADER pImageDosHeader = (PIMAGE_DOS_HEADER)(pMemModule->lpBase);

	PIMAGE_NT_HEADERS pImageNtHeader = MakePointer(
		PIMAGE_NT_HEADERS32, pImageDosHeader, pImageDosHeader->e_lfanew);

	int nNumberOfSections = pImageNtHeader->FileHeader.NumberOfSections;
	PIMAGE_SECTION_HEADER pImageSectionHeader = MakePointer(
		PIMAGE_SECTION_HEADER, pImageNtHeader, sizeof(IMAGE_NT_HEADERS32));

	DWORD dwSectionBase = NULL;

	for (int i = 0; i < nNumberOfSections; ++i)
	{
		if (0 != pImageSectionHeader[i].VirtualAddress && 0 != pImageSectionHeader[i].SizeOfRawData)
		{
			// get attribute of this section
			// not all conditions are considered			
			DWORD dwSectionMemProtect = 0;
			DWORD dwSectionCharacteristics = pImageSectionHeader[i].Characteristics;
			if (dwSectionCharacteristics & IMAGE_SCN_MEM_EXECUTE)
			{
				dwSectionMemProtect = PAGE_EXECUTE;
				if (dwSectionCharacteristics & IMAGE_SCN_MEM_READ)
				{
					dwSectionMemProtect = PAGE_EXECUTE_READ;
				}

				if (dwSectionCharacteristics & IMAGE_SCN_MEM_WRITE)
				{
					dwSectionMemProtect = PAGE_EXECUTE_READWRITE;
				}
			}
			else
			{
				if (dwSectionCharacteristics & IMAGE_SCN_MEM_READ)
				{
					dwSectionMemProtect = PAGE_READONLY;
				}

				if (dwSectionCharacteristics & IMAGE_SCN_MEM_WRITE)
				{
					dwSectionMemProtect = PAGE_READWRITE;
				}
			}

			if (dwSectionCharacteristics & IMAGE_SCN_MEM_NOT_CACHED)
			{
				dwSectionMemProtect |= PAGE_NOCACHE;
			}

			dwSectionBase = pImageSectionHeader[i].VirtualAddress + (DWORD)pMemModule->lpBase;

			// commit memory
			DWORD dwOldMemProtect = 0;
			LPVOID lpSectionBase = NULL;
			br = pfnVirtualProtect(
				(LPVOID)dwSectionBase,
				pImageSectionHeader[i].SizeOfRawData,
				dwSectionMemProtect,
				&dwOldMemProtect);

			if (!br)
			{
				return FALSE;
			}
		}
	}

	return TRUE;
}

BOOL CallModuleEntry(PMEM_MODULE pMemModule, DWORD dwReason)
{
	if (NULL == pMemModule
		|| NULL == pMemModule->pImageDosHeader)
	{
		return FALSE;
	}

	PIMAGE_NT_HEADERS32 pImageNtHeader = MakePointer(
		PIMAGE_NT_HEADERS32,
		pMemModule->pImageDosHeader,
		pMemModule->pImageDosHeader->e_lfanew);

	typedef BOOL(WINAPI * Type_DllMain)(HMODULE, DWORD, LPVOID);

	Type_DllMain pfnModuleEntry = NULL;

	pfnModuleEntry = MakePointer(
		Type_DllMain,
		pMemModule->lpBase,
		pImageNtHeader->OptionalHeader.AddressOfEntryPoint);

	if (NULL == pfnModuleEntry)
	{
		return FALSE;
	}

	return pfnModuleEntry(pMemModule->hModule, dwReason, NULL);
}

FARPROC GetExportedProcAddress(PMEM_MODULE pMemModule, LPCSTR lpName)
{
	if (NULL == pMemModule
		|| NULL == pMemModule->pImageDosHeader)
	{
		return NULL;
	}

	PIMAGE_NT_HEADERS32 pImageNtHeader = MakePointer(
		PIMAGE_NT_HEADERS32,
		pMemModule->pImageDosHeader,
		pMemModule->pImageDosHeader->e_lfanew);

	PIMAGE_EXPORT_DIRECTORY pImageExportDirectory = MakePointer(
		PIMAGE_EXPORT_DIRECTORY,
		pMemModule->lpBase,
		pImageNtHeader->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress);

	PDWORD pAddressOfNames = MakePointer(
		PDWORD, pMemModule->lpBase, pImageExportDirectory->AddressOfNames);

	PWORD pAddressOfNameOrdinals = MakePointer(
		PWORD, pMemModule->lpBase, pImageExportDirectory->AddressOfNameOrdinals);

	PDWORD pAddressOfFunctions = MakePointer(
		PDWORD, pMemModule->lpBase, pImageExportDirectory->AddressOfFunctions);

	int nNumberOfFunctions = pImageExportDirectory->NumberOfFunctions;
	for (int i = 0; i < nNumberOfFunctions; ++i)
	{
		DWORD dwAddressOfName = pAddressOfNames[i];

		LPCSTR pFunctionName = MakePointer(
			LPCSTR, pMemModule->lpBase, dwAddressOfName);

		if (0 == Dw_strcmpA(lpName, pFunctionName))
		{
			WORD wOrdinal = pAddressOfNameOrdinals[i];
			DWORD dwFunctionOffset = pAddressOfFunctions[wOrdinal];
			FARPROC pfnTargetProc = MakePointer(
				FARPROC, pMemModule->lpBase, dwFunctionOffset);

			return pfnTargetProc;
		}
	}

	return NULL;
}

VOID UnmapMemModule(PMEM_MODULE pMemModule)
{
	if (NULL == pMemModule
		|| NULL == pMemModule->pNtFuncptrsTable
		|| FALSE == pMemModule->bLoadOk
		|| NULL == pMemModule->lpBase)
	{
		return;
	}

	typedef BOOL(WINAPI * Type_VirtualFree)(LPVOID, SIZE_T, DWORD);

	Type_VirtualFree pfnVirtualFree = (Type_VirtualFree)(pMemModule->pNtFuncptrsTable->pfnVirtualFree);

	pfnVirtualFree(pMemModule->lpBase, 0, MEM_RELEASE);

	pMemModule->lpBase = NULL;
	pMemModule->dwCrc = 0;
	pMemModule->dwSizeOfImage = 0;
	pMemModule->bLoadOk = FALSE;

	Dw_memset(pMemModule->tszModuleName, 0, sizeof(pMemModule->tszModuleName));
}

//////////////////////////////////////////////////////////////////////////
#include "strmem.cxx"
#include "crc.cxx"

void __declspec(naked) mmLoaderSCEnd()
{
	__asm int 3;
	_asm _emit 'm';
	_asm _emit 'm';
	_asm _emit 'L';
	_asm _emit 'o';
	_asm _emit 'a';
	_asm _emit 'd';
	_asm _emit 'e';
	_asm _emit 'r';
	_asm _emit 'S';
	_asm _emit 'C';
	_asm _emit 'E';
	_asm _emit 'n';
	_asm _emit 'd';
}
