/*
 * vp.h - the subset of the Windows XP video port (videoprt.sys) interface
 * this miniport uses.  Written so the driver builds with a stock Visual
 * Studio + Windows SDK, no DDK/WDK required.
 */
#ifndef GCN_VP_H
#define GCN_VP_H

#include <windows.h>
#include <winioctl.h>
#include <ntddvdeo.h>

typedef LONG VP_STATUS;
typedef LARGE_INTEGER PHYSICAL_ADDRESS, *PPHYSICAL_ADDRESS;

#define VP_PCI_BUS            5   /* INTERFACE_TYPE PCIBus */
#define VP_PCI_CONFIGURATION  4   /* BUS_DATA_TYPE PCIConfiguration */

#define VIDEO_MEMORY_SPACE_MEMORY    0x00
#define VIDEO_MEMORY_SPACE_IO        0x01
#define VIDEO_MEMORY_SPACE_USER_MODE 0x02
#define VIDEO_MEMORY_SPACE_DENSE     0x04
#define VIDEO_MEMORY_SPACE_P6CACHE   0x08

#define VIDEO_ENUM_MORE_DEVICES    ERROR_CONTINUE
#define VIDEO_ENUM_NO_MORE_DEVICES ERROR_NO_MORE_DEVICES

#define DISPLAY_ADAPTER_HW_ID 0xFFFFFFFF

typedef enum _VP_POOL_TYPE {
    VpNonPagedPool,
    VpPagedPool,
    VpNonPagedPoolCacheAligned = 4,
    VpPagedPoolCacheAligned
} VP_POOL_TYPE;

typedef enum _VIDEO_PORT_SERVICES {
    VideoPortServicesAGP = 1,
    VideoPortServicesI2C,
    VideoPortServicesHeadless,
    VideoPortServicesInt10,
    VideoPortServicesDebugReport,
    VideoPortServicesWCMemoryProtection
} VIDEO_PORT_SERVICES;

typedef enum _VIDEO_CHILD_TYPE {
    Monitor = 1,
    NonPrimaryChip,
    VideoChip,
    Other
} VIDEO_CHILD_TYPE, *PVIDEO_CHILD_TYPE;

typedef enum _VIDEO_POWER_STATE_VP {
    VpPowerUnspecified = 0,
    VpPowerOn,
    VpPowerStandBy,
    VpPowerSuspend,
    VpPowerOff,
    VpPowerHibernate,
    VpPowerShutdown
} VIDEO_POWER_STATE_VP;

typedef struct _VIDEO_PORT_CONFIG_INFO {
    ULONG Length;
    ULONG SystemIoBusNumber;
    ULONG AdapterInterfaceType;
    ULONG BusInterruptLevel;
    ULONG BusInterruptVector;
    ULONG InterruptMode;
    ULONG NumEmulatorAccessEntries;
    PVOID EmulatorAccessEntries;
    ULONG_PTR EmulatorAccessEntriesContext;
    PHYSICAL_ADDRESS VdmPhysicalVideoMemoryAddress;
    ULONG VdmPhysicalVideoMemoryLength;
    ULONG HardwareStateSize;
    /* remaining fields are not used by this driver */
} VIDEO_PORT_CONFIG_INFO, *PVIDEO_PORT_CONFIG_INFO;

typedef struct _VIDEO_ACCESS_RANGE {
    PHYSICAL_ADDRESS RangeStart;
    ULONG RangeLength;
    UCHAR RangeInIoSpace;
    UCHAR RangeVisible;
    UCHAR RangeShareable;
    UCHAR RangePassive;
} VIDEO_ACCESS_RANGE, *PVIDEO_ACCESS_RANGE;

typedef struct _STATUS_BLOCK {
    union {
        VP_STATUS Status;
        PVOID Pointer;
    };
    ULONG_PTR Information;
} STATUS_BLOCK, *PSTATUS_BLOCK;

typedef struct _VIDEO_REQUEST_PACKET {
    ULONG IoControlCode;
    PSTATUS_BLOCK StatusBlock;
    PVOID InputBuffer;
    ULONG InputBufferLength;
    PVOID OutputBuffer;
    ULONG OutputBufferLength;
} VIDEO_REQUEST_PACKET, *PVIDEO_REQUEST_PACKET;

typedef struct _VIDEO_CHILD_ENUM_INFO {
    ULONG Size;
    ULONG ChildDescriptorSize;
    ULONG ChildIndex;
    ULONG ACPIHwId;
    PVOID ChildHwDeviceExtension;
} VIDEO_CHILD_ENUM_INFO, *PVIDEO_CHILD_ENUM_INFO;

typedef struct _INT10_BIOS_ARGUMENTS {
    ULONG Eax;
    ULONG Ebx;
    ULONG Ecx;
    ULONG Edx;
    ULONG Esi;
    ULONG Edi;
    ULONG Ebp;
    USHORT SegDs;
    USHORT SegEs;
} INT10_BIOS_ARGUMENTS, *PINT10_BIOS_ARGUMENTS;

typedef VP_STATUS (NTAPI *PINT10_ALLOCATE_BUFFER)(PVOID Context, PUSHORT Seg,
                                                  PUSHORT Off, PULONG Length);
typedef VP_STATUS (NTAPI *PINT10_FREE_BUFFER)(PVOID Context, USHORT Seg,
                                              USHORT Off);
typedef VP_STATUS (NTAPI *PINT10_READ_MEMORY)(PVOID Context, USHORT Seg,
                                              USHORT Off, PVOID Buffer,
                                              ULONG Length);
typedef VP_STATUS (NTAPI *PINT10_WRITE_MEMORY)(PVOID Context, USHORT Seg,
                                               USHORT Off, PVOID Buffer,
                                               ULONG Length);
typedef VP_STATUS (NTAPI *PINT10_CALL_BIOS)(PVOID Context,
                                            PINT10_BIOS_ARGUMENTS Args);

typedef struct _VIDEO_PORT_INT10_INTERFACE {
    USHORT Size;
    USHORT Version;
    PVOID Context;
    PVOID InterfaceReference;
    PVOID InterfaceDereference;
    PINT10_ALLOCATE_BUFFER Int10AllocateBuffer;
    PINT10_FREE_BUFFER Int10FreeBuffer;
    PINT10_READ_MEMORY Int10ReadMemory;
    PINT10_WRITE_MEMORY Int10WriteMemory;
    PINT10_CALL_BIOS Int10CallBios;
} VIDEO_PORT_INT10_INTERFACE, *PVIDEO_PORT_INT10_INTERFACE;

typedef VP_STATUS (NTAPI *PVIDEO_HW_FIND_ADAPTER)(PVOID Ext, PVOID HwContext,
                                                  PWSTR ArgumentString,
                                                  PVIDEO_PORT_CONFIG_INFO Cfg,
                                                  PUCHAR Again);
typedef BOOLEAN (NTAPI *PVIDEO_HW_INITIALIZE)(PVOID Ext);
typedef BOOLEAN (NTAPI *PVIDEO_HW_START_IO)(PVOID Ext,
                                            PVIDEO_REQUEST_PACKET Rp);
typedef BOOLEAN (NTAPI *PVIDEO_HW_RESET_HW)(PVOID Ext, ULONG Columns,
                                            ULONG Rows);
typedef VP_STATUS (NTAPI *PVIDEO_HW_POWER_SET)(PVOID Ext, ULONG HwId,
                                               PVIDEO_POWER_MANAGEMENT Pm);
typedef VP_STATUS (NTAPI *PVIDEO_HW_POWER_GET)(PVOID Ext, ULONG HwId,
                                               PVIDEO_POWER_MANAGEMENT Pm);
typedef ULONG (NTAPI *PVIDEO_HW_GET_CHILD_DESCRIPTOR)(
    PVOID Ext, PVIDEO_CHILD_ENUM_INFO EnumInfo, PVIDEO_CHILD_TYPE Type,
    PUCHAR Descriptor, PULONG UId, PULONG Unused);

typedef struct _VIDEO_HW_INITIALIZATION_DATA {
    ULONG HwInitDataSize;
    ULONG AdapterInterfaceType;
    PVIDEO_HW_FIND_ADAPTER HwFindAdapter;
    PVIDEO_HW_INITIALIZE HwInitialize;
    PVOID HwInterrupt;
    PVIDEO_HW_START_IO HwStartIO;
    ULONG HwDeviceExtensionSize;
    ULONG StartingDeviceNumber;
    PVIDEO_HW_RESET_HW HwResetHw;
    PVOID HwTimer;
    PVOID HwStartDma;
    PVIDEO_HW_POWER_SET HwSetPowerState;
    PVIDEO_HW_POWER_GET HwGetPowerState;
    PVIDEO_HW_GET_CHILD_DESCRIPTOR HwGetVideoChildDescriptor;
    PVOID HwQueryInterface;
    ULONG HwChildDeviceExtensionSize;
    PVIDEO_ACCESS_RANGE HwLegacyResourceList;
    ULONG HwLegacyResourceCount;
    PVOID HwGetLegacyResources;
    BOOLEAN AllowEarlyEnumeration;
    ULONG Reserved;
} VIDEO_HW_INITIALIZATION_DATA, *PVIDEO_HW_INITIALIZATION_DATA;

ULONG NTAPI VideoPortInitialize(PVOID Arg1, PVOID Arg2,
                                PVIDEO_HW_INITIALIZATION_DATA HwInitData,
                                PVOID HwContext);
VP_STATUS NTAPI VideoPortGetAccessRanges(PVOID Ext, ULONG NumRequested,
                                         PVOID Requested, ULONG NumRanges,
                                         PVIDEO_ACCESS_RANGE Ranges,
                                         PVOID VendorId, PVOID DeviceId,
                                         PULONG Slot);
ULONG NTAPI VideoPortGetBusData(PVOID Ext, ULONG BusDataType, ULONG Slot,
                                PVOID Buffer, ULONG Offset, ULONG Length);
PVOID NTAPI VideoPortGetDeviceBase(PVOID Ext, PHYSICAL_ADDRESS IoAddress,
                                   ULONG Length, UCHAR InIoSpace);
VOID NTAPI VideoPortFreeDeviceBase(PVOID Ext, PVOID Mapped);
VP_STATUS NTAPI VideoPortMapMemory(PVOID Ext, PHYSICAL_ADDRESS Phys,
                                   PULONG Length, PULONG InIoSpace,
                                   PVOID *VirtualAddress);
VP_STATUS NTAPI VideoPortUnmapMemory(PVOID Ext, PVOID VirtualAddress,
                                     HANDLE ProcessHandle);
VP_STATUS NTAPI VideoPortQueryServices(PVOID Ext, VIDEO_PORT_SERVICES Type,
                                       PVOID Interface);
PVOID NTAPI VideoPortAllocatePool(PVOID Ext, VP_POOL_TYPE Type, SIZE_T Bytes,
                                  ULONG Tag);
VOID NTAPI VideoPortFreePool(PVOID Ext, PVOID Ptr);
VP_STATUS NTAPI VideoPortSetRegistryParameters(PVOID Ext, PWSTR ValueName,
                                               PVOID ValueData,
                                               ULONG ValueLength);
VOID NTAPI VideoPortStallExecution(ULONG Microseconds);
LONGLONG NTAPI VideoPortQueryPerformanceCounter(PVOID Ext,
                                                PLONGLONG Frequency);

typedef VP_STATUS (NTAPI *PMINIPORT_GET_REGISTRY_ROUTINE)(
    PVOID Ext, PVOID Context, PWSTR ValueName, PVOID ValueData,
    ULONG ValueLength);
VP_STATUS NTAPI VideoPortGetRegistryParameters(
    PVOID Ext, PWSTR ParameterName, UCHAR IsParameterFileName,
    PMINIPORT_GET_REGISTRY_ROUTINE Callback, PVOID Context);

#endif
