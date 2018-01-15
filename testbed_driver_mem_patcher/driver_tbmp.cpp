// Copyright (c) 2015-2016, tandasat. All rights reserved.
// Use of this source code is governed by a MIT-style license that can be
// found in the LICENSE file.

/// @file
/// Implements an entry point of the driver.

#ifndef POOL_NX_OPTIN
#define POOL_NX_OPTIN 1
#endif
#include "driver.h"


extern "C" {
////////////////////////////////////////////////////////////////////////////////
//
// macro utilities
//

////////////////////////////////////////////////////////////////////////////////
//
// constants and macros
//

////////////////////////////////////////////////////////////////////////////////
//
// types
//

////////////////////////////////////////////////////////////////////////////////
//
// prototypes
//

DRIVER_INITIALIZE DriverEntry;

static DRIVER_UNLOAD DriverpDriverUnload;

_IRQL_requires_max_(PASSIVE_LEVEL) bool DriverpIsSuppoetedOS();

#if defined(ALLOC_PRAGMA)
#pragma alloc_text(INIT, DriverEntry)
#pragma alloc_text(PAGE, DriverpDriverUnload)
#pragma alloc_text(INIT, DriverpIsSuppoetedOS)
#endif

////////////////////////////////////////////////////////////////////////////////
//
// variables
//

////////////////////////////////////////////////////////////////////////////////
//
// implementations
//



void remove_symbol_link(PWCHAR linkName){
	UNICODE_STRING device_link;
	RtlInitUnicodeString(&device_link, linkName);
	IoDeleteSymbolicLink(&device_link);
}

void remove_control_device(PDRIVER_OBJECT driver_object) {
	PDEVICE_OBJECT device_object = driver_object->DeviceObject;
	while (device_object){
		IoDeleteDevice(device_object);
		device_object = device_object->NextDevice;
	}
}

// Unload handler
_Use_decl_annotations_ static void DriverpDriverUnload(
    PDRIVER_OBJECT driver_object) {
  UNREFERENCED_PARAMETER(driver_object);
  PAGED_CODE();
  //TESTBED_COMMON_DBG_BREAK();

  remove_symbol_link(TESTBEDMP_LINKNAME_APP);
  remove_control_device(driver_object);

  DbgPrint("The driver [%ws] has been unloaded! \r\n", TESTBEDMP_SYS_FILE);
}

// Create-Close handler
_Use_decl_annotations_ NTSTATUS DriverpCreateClose(IN PDEVICE_OBJECT pDeviceObject, IN PIRP  Irp) {
	UNREFERENCED_PARAMETER(pDeviceObject);
	PAGED_CODE();

	const auto stack = IoGetCurrentIrpStackLocation(Irp);
	switch (stack->MajorFunction) {
		case IRP_MJ_CREATE: break;
		case IRP_MJ_CLOSE: break;
	}
	Irp->IoStatus.Status = STATUS_SUCCESS;
	Irp->IoStatus.Information = 0;
	IoCompleteRequest(Irp, 0);
	return STATUS_SUCCESS;
}


// Read Write handler
_Use_decl_annotations_ static NTSTATUS DriverpReadWrite(IN PDEVICE_OBJECT pDeviceObject, IN PIRP  Irp){
	PAGED_CODE();

	PVOID buf = NULL;
	auto buf_size = 0;
	// Read size of input buffer 
	const auto stack = IoGetCurrentIrpStackLocation(Irp);
	switch (stack->MajorFunction){
		case IRP_MJ_READ: buf_size = stack->Parameters.Read.Length; break;
		case IRP_MJ_WRITE: buf_size = stack->Parameters.Write.Length; break;
	}
	// Get the address of input buffer
	if (buf_size){
		if (pDeviceObject->Flags & DO_BUFFERED_IO) {
			buf = Irp->AssociatedIrp.SystemBuffer;
		}
		else if (pDeviceObject->Flags & DO_DIRECT_IO) {
			buf = MmGetSystemAddressForMdlSafe(Irp->MdlAddress, NormalPagePriority);
		}
		else {
			buf = Irp->UserBuffer;
		}
	}

	// Do nothing and complete request
	Irp->IoStatus.Information = 0;
	Irp->IoStatus.Status = STATUS_SUCCESS;
	IoCompleteRequest(Irp, IO_NO_INCREMENT);
	return STATUS_SUCCESS;
}

_Use_decl_annotations_ static void read_param(IN PIRP pIrp, 
	OUT PVOID &inBuf, OUT ULONG &inBufSize, 
	OUT PVOID &outBuf, OUT ULONG &outBufSize){
	const auto stack = IoGetCurrentIrpStackLocation(pIrp);
	inBufSize = stack->Parameters.DeviceIoControl.InputBufferLength;
	outBufSize = stack->Parameters.DeviceIoControl.OutputBufferLength;
	const auto method = stack->Parameters.DeviceIoControl.IoControlCode & 0x03L;
	switch (method)
	{
	case METHOD_BUFFERED:
		inBuf = pIrp->AssociatedIrp.SystemBuffer;
		outBuf = pIrp->AssociatedIrp.SystemBuffer;
		break;
	case METHOD_IN_DIRECT:
		inBuf = pIrp->AssociatedIrp.SystemBuffer;
		outBuf = MmGetSystemAddressForMdlSafe(pIrp->MdlAddress, NormalPagePriority);
		break;
	case METHOD_OUT_DIRECT:
		inBuf = pIrp->AssociatedIrp.SystemBuffer;
		outBuf = MmGetSystemAddressForMdlSafe(pIrp->MdlAddress, NormalPagePriority);
		break;
	case METHOD_NEITHER:
		inBuf = stack->Parameters.DeviceIoControl.Type3InputBuffer;
		outBuf = pIrp->UserBuffer;
		break;
	}
}

void hide_proc(const ULONG64 procID) {

	const int apl_offset = 0x2e8;
	const int pid_offset = 0x2e0;

	PLIST_ENTRY current_apl = (PLIST_ENTRY) ((char*)PsInitialSystemProcess + apl_offset);
	const PLIST_ENTRY begin_apl = current_apl;

	ULONG64 curret_pid = 0;

	do
	{
		curret_pid = *(ULONG64*)( (char*)current_apl - apl_offset + pid_offset);
		if (procID == curret_pid){
			RemoveEntryList(current_apl);
			break;
		}
		current_apl = current_apl->Flink;
	} while (begin_apl != current_apl);
}

// IOCTL dispatch handler
_Use_decl_annotations_ static NTSTATUS DriverpDeviceControl(IN PDEVICE_OBJECT pDeviceObject, IN PIRP pIrp) {
	UNREFERENCED_PARAMETER(pDeviceObject);
	PAGED_CODE();

	ULONG_PTR LowLimit = { 0 };
	ULONG_PTR HighLimit = { 0 };
	IoGetStackLimits(&LowLimit, &HighLimit);
	DbgPrint("Stack limits [DriverpDeviceControl] %I64X-%I64X \r\n",
		LowLimit, HighLimit);

	const auto stack = IoGetCurrentIrpStackLocation(pIrp);
	PVOID in_buf = NULL, out_buf = NULL;
	ULONG in_buf_sz = 0, out_buf_sz = 0;
	auto status = STATUS_INVALID_PARAMETER;
	ULONG_PTR info = 0;
	read_param(pIrp, in_buf, in_buf_sz, out_buf, out_buf_sz);
	switch (stack->Parameters.DeviceIoControl.IoControlCode)
	{
		case TESTBED_MEM_PATCHER_HIDE_PROC:
			if (in_buf_sz == sizeof ULONG64) {
				ULONG64 pid = 0;
				ULONG64* pdata = (ULONG64*)in_buf;
				__try {
					pid = *pdata;
				}
				__except (EXCEPTION_EXECUTE_HANDLER) { pid = 0; }
				if (pid){
					hide_proc(pid);
				}
			}
			break;
		case TESTBED_MEM_PATCHER_READ_1_BYTE:
			if (in_buf_sz == sizeof ADDR_BYTE) {
				ADDR_BYTE* pdata = (ADDR_BYTE*)in_buf;
				__try {
					pdata->value = *(char*)pdata->addr;
				}
				__except (EXCEPTION_EXECUTE_HANDLER) {   }
			}
			break;
		case TESTBED_MEM_PATCHER_WRITE_1_BYTE:
			if (in_buf_sz == sizeof ADDR_BYTE) {
				ADDR_BYTE* pdata = (ADDR_BYTE*)in_buf;
				__try {
					*(char*)pdata->addr = pdata->value;
				}
				__except (EXCEPTION_EXECUTE_HANDLER) {   }
			}
			break;
		case TESTBED_MEM_PATCHER_WRITE_8_BYTES:
			if (in_buf_sz == sizeof ADDR_8BYTES) {
				ADDR_8BYTES* pdata = (ADDR_8BYTES*)in_buf;
				__try {
					*(ULONG64*)pdata->addr = pdata->value;
				}
				__except (EXCEPTION_EXECUTE_HANDLER) {   }
			}
			break;
		default: {}
	}

	pIrp->IoStatus.Information = info;
	pIrp->IoStatus.Status = status;
	IoCompleteRequest(pIrp, IO_NO_INCREMENT);
	return STATUS_SUCCESS;
}

// Test if the system is one of supported OS versions
_Use_decl_annotations_ bool DriverpIsSuppoetedOS() {
  PAGED_CODE();

  RTL_OSVERSIONINFOW os_version = {};
  auto status = RtlGetVersion(&os_version);
  if (!NT_SUCCESS(status)) {
    return false;
  }

  if (os_version.dwBuildNumber != 15063){
	  return false;
  }

  // 4-gigabyte tuning (4GT) should not be enabled
  if (!IsX64() &&
      reinterpret_cast<ULONG_PTR>(MmSystemRangeStart) != 0x80000000) {
    return false;
  }
  return true;
}

_Use_decl_annotations_ NTSTATUS create_device(IN PDRIVER_OBJECT pDrv, ULONG uFlags, PWCHAR devName, PWCHAR linkName)
{
	UNICODE_STRING dev_name = { 0 }, link_name = {0};
	RtlInitUnicodeString(&dev_name, devName);
	RtlInitUnicodeString(&link_name, linkName);

	PDEVICE_OBJECT pDev;
	auto status = IoCreateDevice(pDrv, 0 /* or sizeof(DEVICE_EXTENSION)*/, &dev_name, 65500, 0, 0, &pDev);

	if (NT_SUCCESS(status)) {
		pDev->Flags |= uFlags;
		IoDeleteSymbolicLink(&link_name);
		status = IoCreateSymbolicLink(&link_name, &dev_name);
	}
	else   {   IoDeleteDevice(pDev);   }

	return status;
}

#include "intrin.h"
static const unsigned long SMEP_MASK = 0x100000;

void print_smep_status() {
	bool b_active = false;
	KAFFINITY active_processors = KeQueryActiveProcessors();
	for (KAFFINITY current_affinity = 1; active_processors; current_affinity <<= 1) {
		if (active_processors & current_affinity) {
			active_processors &= ~current_affinity;
			KeSetSystemAffinityThread(current_affinity);
			b_active = (0 != (__readcr4() & SMEP_MASK));
			DbgPrint("%s on CPU %d \r\n",
				b_active ? "SMEP is active" : "SMEP has been disabled",
				KeGetCurrentProcessorNumber() );
		}
	}
}

// A driver entry point
_Use_decl_annotations_ NTSTATUS DriverEntry(PDRIVER_OBJECT driver_object,
	PUNICODE_STRING registry_path) {
	UNREFERENCED_PARAMETER(registry_path);
	PAGED_CODE();
	
	DbgPrint("********************************* \r\n");
	DbgPrint("The driver [%ws] has been loaded to %I64X-%I64X \r\n", 
		driver_object->DriverName.Buffer,
		driver_object->DriverStart, (char*)driver_object->DriverStart+ driver_object->DriverSize);
	
	DbgPrint("\r\n********************************* \r\n");

	// Test if the system is supported
// 	if (!DriverpIsSuppoetedOS()) {
// 		return STATUS_CANCELLED;
// 	}

//	print_smep_status();

	driver_object->DriverUnload = DriverpDriverUnload;
	driver_object->MajorFunction[IRP_MJ_CREATE] =
	driver_object->MajorFunction[IRP_MJ_CLOSE] = DriverpCreateClose;
	driver_object->MajorFunction[IRP_MJ_READ] =
	driver_object->MajorFunction[IRP_MJ_WRITE] = DriverpReadWrite;
	driver_object->MajorFunction[IRP_MJ_DEVICE_CONTROL] = DriverpDeviceControl;

	auto nt_status = create_device(driver_object, NULL, TESTBEDMP_DEVICENAME_DRV, TESTBEDMP_LINKNAME_DRV);

	return nt_status;
}

}  // extern "C"
