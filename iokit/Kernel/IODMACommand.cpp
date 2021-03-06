/*
 * Copyright (c) 2005-2006 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 * 
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. The rights granted to you under the License
 * may not be used to create, or enable the creation or redistribution of,
 * unlawful or unlicensed copies of an Apple operating system, or to
 * circumvent, violate, or enable the circumvention or violation of, any
 * terms of an Apple operating system software license agreement.
 * 
 * Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this file.
 * 
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 * 
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */

#include <IOKit/assert.h>

#include <libkern/OSTypes.h>
#include <libkern/OSByteOrder.h>
#include <libkern/OSDebug.h>

#include <IOKit/IOReturn.h>
#include <IOKit/IOLib.h>
#include <IOKit/IODMACommand.h>
#include <IOKit/IOMapper.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>

#include "IOKitKernelInternal.h"

#define MAPTYPE(type)		((UInt) (type) & kTypeMask)
#define IS_MAPPED(type)		(MAPTYPE(type) != kBypassed)
#define IS_BYPASSED(type)	(MAPTYPE(type) == kBypassed)
#define IS_NONCOHERENT(type)	(MAPTYPE(type) == kNonCoherent)

enum 
{
    kWalkSyncIn       = 0x01,	// bounce -> md 
    kWalkSyncOut      = 0x02,	// bounce <- md
    kWalkSyncAlways   = 0x04,
    kWalkPreflight    = 0x08,
    kWalkDoubleBuffer = 0x10,
    kWalkPrepare      = 0x20,
    kWalkComplete     = 0x40,
    kWalkClient       = 0x80
};


#define fInternalState reserved
#define fState         reserved->fState
#define fMDSummary     reserved->fMDSummary


#if 1
// no direction => OutIn
#define SHOULD_COPY_DIR(op, direction)					    \
	((kIODirectionNone == (direction))				    \
	    || (kWalkSyncAlways & (op))					    \
	    || (((kWalkSyncIn & (op)) ? kIODirectionIn : kIODirectionOut)   \
						    & (direction)))

#else
#define SHOULD_COPY_DIR(state, direction) (true)
#endif

#if 0
#define DEBG(fmt, args...)	{ IOLog(fmt, ## args); kprintf(fmt, ## args); }
#else
#define DEBG(fmt, args...)  	{}
#endif

/**************************** class IODMACommand ***************************/

#undef super
#define super IOCommand
OSDefineMetaClassAndStructors(IODMACommand, IOCommand);

OSMetaClassDefineReservedUsed(IODMACommand,  0);
OSMetaClassDefineReservedUsed(IODMACommand,  1);
OSMetaClassDefineReservedUsed(IODMACommand,  2);
OSMetaClassDefineReservedUnused(IODMACommand,  3);
OSMetaClassDefineReservedUnused(IODMACommand,  4);
OSMetaClassDefineReservedUnused(IODMACommand,  5);
OSMetaClassDefineReservedUnused(IODMACommand,  6);
OSMetaClassDefineReservedUnused(IODMACommand,  7);
OSMetaClassDefineReservedUnused(IODMACommand,  8);
OSMetaClassDefineReservedUnused(IODMACommand,  9);
OSMetaClassDefineReservedUnused(IODMACommand, 10);
OSMetaClassDefineReservedUnused(IODMACommand, 11);
OSMetaClassDefineReservedUnused(IODMACommand, 12);
OSMetaClassDefineReservedUnused(IODMACommand, 13);
OSMetaClassDefineReservedUnused(IODMACommand, 14);
OSMetaClassDefineReservedUnused(IODMACommand, 15);

IODMACommand *
IODMACommand::withSpecification(SegmentFunction outSegFunc,
				UInt8           numAddressBits,
				UInt64          maxSegmentSize,
				MappingOptions  mappingOptions,
				UInt64          maxTransferSize,
				UInt32          alignment,
				IOMapper       *mapper,
				void           *refCon)
{
    IODMACommand * me = new IODMACommand;

    if (me && !me->initWithSpecification(outSegFunc,
					 numAddressBits, maxSegmentSize,
					 mappingOptions, maxTransferSize,
					 alignment,      mapper, refCon))
    {
        me->release();
        return 0;
    };

    return me;
}

IODMACommand *
IODMACommand::cloneCommand(void *refCon)
{
    return withSpecification(fOutSeg, fNumAddressBits, fMaxSegmentSize,
	    fMappingOptions, fMaxTransferSize, fAlignMask + 1, fMapper, refCon);
}

#define kLastOutputFunction ((SegmentFunction) kLastOutputFunction)

bool
IODMACommand::initWithSpecification(SegmentFunction outSegFunc,
				    UInt8           numAddressBits,
				    UInt64          maxSegmentSize,
				    MappingOptions  mappingOptions,
				    UInt64          maxTransferSize,
				    UInt32          alignment,
				    IOMapper       *mapper,
				    void           *refCon)
{
    IOService * device = 0;

    if (!super::init() || !outSegFunc)
        return false;

    bool is32Bit = (OutputHost32   == outSegFunc || OutputBig32 == outSegFunc
                 || OutputLittle32 == outSegFunc);
    if (is32Bit)
    {
	if (!numAddressBits)
	    numAddressBits = 32;
	else if (numAddressBits > 32)
	    return false;		// Wrong output function for bits
    }

    if (numAddressBits && (numAddressBits < PAGE_SHIFT))
	return false;

    if (!maxSegmentSize)
	maxSegmentSize--;	// Set Max segment to -1
    if (!maxTransferSize)
	maxTransferSize--;	// Set Max transfer to -1


    if (mapper && !OSDynamicCast(IOMapper, mapper))
    {
    	device = mapper;
    	mapper = 0;
    }
    if (!mapper)
    {
        IOMapper::checkForSystemMapper();
	mapper = IOMapper::gSystem;
    }

    fNumSegments     = 0;
    fBypassMask      = 0;
    fOutSeg	     = outSegFunc;
    fNumAddressBits  = numAddressBits;
    fMaxSegmentSize  = maxSegmentSize;
    fMappingOptions  = mappingOptions;
    fMaxTransferSize = maxTransferSize;
    if (!alignment)
	alignment = 1;
    fAlignMask	     = alignment - 1;
    fMapper          = mapper;
    fRefCon          = refCon;

    switch (MAPTYPE(mappingOptions))
    {
    case kMapped:                   break;
    case kNonCoherent: /*fMapper = 0;*/ break;
    case kBypassed:
	if (mapper && !mapper->getBypassMask(&fBypassMask))
	    return false;
	break;
    default:
	return false;
    };

    if (fMapper)
	fMapper->retain();

    reserved = IONew(IODMACommandInternal, 1);
    if (!reserved)
	return false;
    bzero(reserved, sizeof(IODMACommandInternal));

    fInternalState->fIterateOnly = (0 != (kIterateOnly & mappingOptions));
    fInternalState->fDevice = device;

    return true;
}

void
IODMACommand::free()
{
    if (reserved)
	IODelete(reserved, IODMACommandInternal, 1);

    if (fMapper)
	fMapper->release();

    super::free();
}

IOReturn
IODMACommand::setMemoryDescriptor(const IOMemoryDescriptor *mem, bool autoPrepare)
{
	IOReturn err = kIOReturnSuccess;
	
    if (mem == fMemory)
    {
	if (!autoPrepare)
	{
	    while (fActive)
		complete();
	}
	return kIOReturnSuccess;
    }

    if (fMemory) {
	// As we are almost certainly being called from a work loop thread
	// if fActive is true it is probably not a good time to potentially
	// block.  Just test for it and return an error
	if (fActive)
	    return kIOReturnBusy;
	clearMemoryDescriptor();
    }

    if (mem) {
	bzero(&fMDSummary, sizeof(fMDSummary));
	err = mem->dmaCommandOperation(kIOMDGetCharacteristics | (kMapped == MAPTYPE(fMappingOptions)),
				       &fMDSummary, sizeof(fMDSummary));
	if (err)
	    return err;

	ppnum_t highPage = fMDSummary.fHighestPage ? fMDSummary.fHighestPage : gIOLastPage;

	if ((kMapped == MAPTYPE(fMappingOptions))
	    && fMapper)
	    fInternalState->fCheckAddressing = false;
	else
	    fInternalState->fCheckAddressing = (fNumAddressBits && (highPage >= (1UL << (fNumAddressBits - PAGE_SHIFT))));

	fInternalState->fNewMD = true;
	mem->retain();
	fMemory = mem;

	mem->dmaCommandOperation(kIOMDSetDMAActive, this, 0);
	if (autoPrepare) {
	    err = prepare();
	    if (err) {
		clearMemoryDescriptor();
	    }
	}
    }
	
    return err;
}

IOReturn
IODMACommand::clearMemoryDescriptor(bool autoComplete)
{
    if (fActive && !autoComplete)
	return (kIOReturnNotReady);

    if (fMemory) {
	while (fActive)
	    complete();
	fMemory->dmaCommandOperation(kIOMDSetDMAInactive, this, 0);
	fMemory->release();
	fMemory = 0;
    }

    return (kIOReturnSuccess);
}

const IOMemoryDescriptor *
IODMACommand::getMemoryDescriptor() const
{
    return fMemory;
}


IOReturn
IODMACommand::segmentOp(
			void         *reference,
			IODMACommand *target,
			Segment64     segment,
			void         *segments,
			UInt32        segmentIndex)
{
    IOOptionBits op = (uintptr_t) reference;
    addr64_t     maxPhys, address;
    uint64_t     length;
    uint32_t     numPages;

    IODMACommandInternal * state = target->reserved;

    if (target->fNumAddressBits && (target->fNumAddressBits < 64) && (state->fLocalMapperPageAlloc || !target->fMapper))
	maxPhys = (1ULL << target->fNumAddressBits);
    else
	maxPhys = 0;
    maxPhys--;

    address = segment.fIOVMAddr;
    length = segment.fLength;

    assert(address);
    assert(length);

    if (!state->fMisaligned)
    {
	state->fMisaligned |= (0 != (state->fSourceAlignMask & address));
	if (state->fMisaligned) DEBG("misaligned %qx:%qx, %lx\n", address, length, state->fSourceAlignMask);
    }

    if (state->fMisaligned && (kWalkPreflight & op))
	return (kIOReturnNotAligned);

    if (!state->fDoubleBuffer)
    {
	if ((address + length - 1) <= maxPhys)
	{
	    length = 0;
	}
	else if (address <= maxPhys)
	{
	    DEBG("tail %qx, %qx", address, length);
	    length = (address + length - maxPhys - 1);
	    address = maxPhys + 1;
	    DEBG("-> %qx, %qx\n", address, length);
	}
    }

    if (!length)
	return (kIOReturnSuccess);

    numPages = atop_64(round_page_64((address & PAGE_MASK) + length));

    if (kWalkPreflight & op)
    {
	state->fCopyPageCount += numPages;
    }
    else
    {
	vm_page_t lastPage;
	lastPage = NULL;
	if (kWalkPrepare & op)
	{
	    lastPage = state->fCopyNext;
	    for (IOItemCount idx = 0; idx < numPages; idx++)
	    {
		vm_page_set_offset(lastPage, atop_64(address) + idx);
		lastPage = vm_page_get_next(lastPage);
	    }
	}

	if (!lastPage || SHOULD_COPY_DIR(op, target->fMDSummary.fDirection))
	{
	    lastPage = state->fCopyNext;
	    for (IOItemCount idx = 0; idx < numPages; idx++)
	    {
		if (SHOULD_COPY_DIR(op, target->fMDSummary.fDirection))
		{
		    addr64_t cpuAddr = address;
		    addr64_t remapAddr;
		    uint64_t chunk;

		    if ((kMapped == MAPTYPE(target->fMappingOptions))
			&& target->fMapper)
		    {
			cpuAddr = target->fMapper->mapAddr(address);
		    }
	
		    remapAddr = ptoa_64(vm_page_get_phys_page(lastPage));
		    if (!state->fDoubleBuffer)
		    {
			remapAddr += (address & PAGE_MASK);
		    }
		    chunk = PAGE_SIZE - (address & PAGE_MASK);
		    if (chunk > length)
			chunk = length;

		    DEBG("cpv: 0x%qx %s 0x%qx, 0x%qx, 0x%02lx\n", remapAddr, 
				(kWalkSyncIn & op) ? "->" : "<-", 
				address, chunk, op);

		    if (kWalkSyncIn & op)
		    { // cppvNoModSnk
			copypv(remapAddr, cpuAddr, chunk,
					cppvPsnk | cppvFsnk | cppvPsrc | cppvNoRefSrc );
		    }
		    else
		    {
			copypv(cpuAddr, remapAddr, chunk,
					cppvPsnk | cppvFsnk | cppvPsrc | cppvNoRefSrc );
		    }
		    address += chunk;
		    length -= chunk;
		}
		lastPage = vm_page_get_next(lastPage);
	    }
	}
	state->fCopyNext = lastPage;
    }

    return kIOReturnSuccess;
}

IOReturn
IODMACommand::walkAll(UInt8 op)
{
    IODMACommandInternal * state = fInternalState;

    IOReturn     ret = kIOReturnSuccess;
    UInt32       numSegments;
    UInt64       offset;

    if (kWalkPreflight & op)
    {
	state->fMisaligned     = false;
	state->fDoubleBuffer   = false;
	state->fPrepared       = false;
	state->fCopyNext       = NULL;
	state->fCopyPageAlloc  = 0;
	state->fCopyPageCount  = 0;
	state->fNextRemapPage  = NULL;
	state->fCopyMD	       = 0;

	if (!(kWalkDoubleBuffer & op))
	{
	    offset = 0;
	    numSegments = 0-1;
	    ret = genIOVMSegments(op, segmentOp, (void *) op, &offset, state, &numSegments);
	}

	op &= ~kWalkPreflight;

	state->fDoubleBuffer = (state->fMisaligned || (kWalkDoubleBuffer & op));
	if (state->fDoubleBuffer)
	    state->fCopyPageCount = atop_64(round_page(state->fPreparedLength));

	if (state->fCopyPageCount)
	{
	    vm_page_t mapBase = NULL;

	    DEBG("preflight fCopyPageCount %d\n", state->fCopyPageCount);

	    if (!state->fDoubleBuffer)
	    {
		kern_return_t kr;

		if (fMapper) panic("fMapper copying");

		kr = vm_page_alloc_list(state->fCopyPageCount, 
					KMA_LOMEM | KMA_NOPAGEWAIT, &mapBase);
		if (KERN_SUCCESS != kr)
		{
		    DEBG("vm_page_alloc_list(%d) failed (%d)\n", state->fCopyPageCount, kr);
		    mapBase = NULL;
		}
	    }

	    if (mapBase)
	    {
		state->fCopyPageAlloc = mapBase;
		state->fCopyNext = state->fCopyPageAlloc;
		offset = 0;
		numSegments = 0-1;
		ret = genIOVMSegments(op, segmentOp, (void *) op, &offset, state, &numSegments);
		state->fPrepared = true;
		op &= ~(kWalkSyncIn | kWalkSyncOut);
	    }
	    else
	    {
		DEBG("alloc IOBMD\n");
		mach_vm_address_t mask = 0xFFFFF000; //state->fSourceAlignMask
		state->fCopyMD = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
				    fMDSummary.fDirection, state->fPreparedLength, mask);

		if (state->fCopyMD)
		{
		    ret = kIOReturnSuccess;
		    state->fPrepared = true;
		}
		else
		{
		    DEBG("IODMACommand !alloc IOBMD");
		    return (kIOReturnNoResources);
		}
	    }
	}
    }

    if (state->fPrepared && ((kWalkSyncIn | kWalkSyncOut) & op))
    {
	if (state->fCopyPageCount)
	{
	    DEBG("sync fCopyPageCount %d\n", state->fCopyPageCount);

	    if (state->fCopyPageAlloc)
	    {
		state->fCopyNext = state->fCopyPageAlloc;
		offset = 0;
		numSegments = 0-1;
		ret = genIOVMSegments(op, segmentOp, (void *) op, &offset, state, &numSegments);
	    }
	    else if (state->fCopyMD)
	    {
		DEBG("sync IOBMD\n");

		if (SHOULD_COPY_DIR(op, fMDSummary.fDirection))
		{
		    IOMemoryDescriptor *poMD = const_cast<IOMemoryDescriptor *>(fMemory);

		    IOByteCount bytes;
		    
		    if (kWalkSyncIn & op)
			bytes = poMD->writeBytes(state->fPreparedOffset, 
						    state->fCopyMD->getBytesNoCopy(),
						    state->fPreparedLength);
		    else
			bytes = poMD->readBytes(state->fPreparedOffset, 
						    state->fCopyMD->getBytesNoCopy(),
						    state->fPreparedLength);
		    DEBG("fCopyMD %s %lx bytes\n", (kWalkSyncIn & op) ? "wrote" : "read", bytes);
		    ret = (bytes == state->fPreparedLength) ? kIOReturnSuccess : kIOReturnUnderrun;
		}
		else
		    ret = kIOReturnSuccess;
	    }
	}
    }

    if (kWalkComplete & op)
    {
	if (state->fCopyPageAlloc)
	{
	    vm_page_free_list(state->fCopyPageAlloc, FALSE);
	    state->fCopyPageAlloc = 0;
	    state->fCopyPageCount = 0;
	}
	if (state->fCopyMD)
	{
	    state->fCopyMD->release();
	    state->fCopyMD = 0;
	}

	state->fPrepared = false;
    }
    return (ret);
}

UInt8
IODMACommand::getNumAddressBits(void)
{
    return (fNumAddressBits);
}

UInt32
IODMACommand::getAlignment(void)
{
    return (fAlignMask + 1);
}

IOReturn
IODMACommand::prepareWithSpecification(SegmentFunction	outSegFunc,
				       UInt8		numAddressBits,
				       UInt64		maxSegmentSize,
				       MappingOptions	mappingOptions,
				       UInt64		maxTransferSize,
				       UInt32		alignment,
				       IOMapper		*mapper,
				       UInt64		offset,
				       UInt64		length,
				       bool		flushCache,
				       bool		synchronize)
{
    if (fActive)
        return kIOReturnNotPermitted;

    if (!outSegFunc)
        return kIOReturnBadArgument;

    bool is32Bit = (OutputHost32   == outSegFunc || OutputBig32 == outSegFunc
                 || OutputLittle32 == outSegFunc);
    if (is32Bit)
    {
	if (!numAddressBits)
	    numAddressBits = 32;
	else if (numAddressBits > 32)
	    return kIOReturnBadArgument;		// Wrong output function for bits
    }

    if (numAddressBits && (numAddressBits < PAGE_SHIFT))
	return kIOReturnBadArgument;

    if (!maxSegmentSize)
	maxSegmentSize--;	// Set Max segment to -1
    if (!maxTransferSize)
	maxTransferSize--;	// Set Max transfer to -1

    if (mapper && !OSDynamicCast(IOMapper, mapper))
    {
    	fInternalState->fDevice = mapper;
    	mapper = 0;
    }
    if (!mapper)
    {
        IOMapper::checkForSystemMapper();
	mapper = IOMapper::gSystem;
    }

    switch (MAPTYPE(mappingOptions))
    {
    case kMapped:                   break;
    case kNonCoherent:              break;
    case kBypassed:
	if (mapper && !mapper->getBypassMask(&fBypassMask))
	    return kIOReturnBadArgument;
	break;
    default:
	return kIOReturnBadArgument;
    };

    fNumSegments     = 0;
    fBypassMask      = 0;
    fOutSeg	     = outSegFunc;
    fNumAddressBits  = numAddressBits;
    fMaxSegmentSize  = maxSegmentSize;
    fMappingOptions  = mappingOptions;
    fMaxTransferSize = maxTransferSize;
    if (!alignment)
	alignment = 1;
    fAlignMask	     = alignment - 1;
    if (mapper != fMapper)
    {
	mapper->retain();
	fMapper->release();
	fMapper = mapper;
    }

    fInternalState->fIterateOnly = (0 != (kIterateOnly & mappingOptions));

    return prepare(offset, length, flushCache, synchronize);
}


IOReturn 
IODMACommand::prepare(UInt64 offset, UInt64 length, bool flushCache, bool synchronize)
{
    IODMACommandInternal * state = fInternalState;
    IOReturn               ret   = kIOReturnSuccess;
    MappingOptions mappingOptions    = fMappingOptions;

    if (!length)
	length = fMDSummary.fLength;

    if (length > fMaxTransferSize)
	return kIOReturnNoSpace;

    if (IS_NONCOHERENT(mappingOptions) && flushCache) {
	IOMemoryDescriptor *poMD = const_cast<IOMemoryDescriptor *>(fMemory);

	poMD->performOperation(kIOMemoryIncoherentIOStore, offset, length);
    }
    if (fActive++)
    {
	if ((state->fPreparedOffset != offset)
	  || (state->fPreparedLength != length))
	ret = kIOReturnNotReady;
    }
    else
    {
	state->fPreparedOffset = offset;
	state->fPreparedLength = length;

	state->fMapContig      = false;
	state->fMisaligned     = false;
	state->fDoubleBuffer   = false;
	state->fPrepared       = false;
	state->fCopyNext       = NULL;
	state->fCopyPageAlloc  = 0;
	state->fCopyPageCount  = 0;
	state->fNextRemapPage  = NULL;
	state->fCopyMD         = 0;
	state->fLocalMapperPageAlloc = 0;
	state->fLocalMapperPageCount = 0;

	state->fLocalMapper    = (fMapper && (fMapper != IOMapper::gSystem));

	state->fSourceAlignMask = fAlignMask;
	if (fMapper)
	    state->fSourceAlignMask &= page_mask;
	
	state->fCursor = state->fIterateOnly
			|| (!state->fCheckAddressing
			    && (!state->fSourceAlignMask
				|| ((fMDSummary.fPageAlign & (1 << 31)) && (0 == (fMDSummary.fPageAlign & state->fSourceAlignMask)))));

	if (!state->fCursor)
	{
	    IOOptionBits op = kWalkPrepare | kWalkPreflight;
	    if (synchronize)
		op |= kWalkSyncOut;
	    ret = walkAll(op);
	}

	if (fMapper)
	{
	    if (state->fLocalMapper)
	    {
		state->fLocalMapperPageCount = atop_64(round_page(
			state->fPreparedLength + ((state->fPreparedOffset + fMDSummary.fPageAlign) & page_mask)));
		state->fLocalMapperPageAlloc = ptoa_64(fMapper->iovmAllocDMACommand(this, state->fLocalMapperPageCount));
		if (!state->fLocalMapperPageAlloc)
		{
		    DEBG("IODMACommand !iovmAlloc");
		    return (kIOReturnNoResources);
		}
		state->fMapContig = true;
	    }
	    else
	    {
		IOMDDMAMapArgs mapArgs;
		bzero(&mapArgs, sizeof(mapArgs));
		mapArgs.fMapper = fMapper;
		mapArgs.fMapSpec.device         = state->fDevice;
		mapArgs.fMapSpec.alignment      = fAlignMask + 1;
		mapArgs.fMapSpec.numAddressBits = fNumAddressBits ? fNumAddressBits : 64;
		mapArgs.fOffset = state->fPreparedOffset;
		mapArgs.fLength = state->fPreparedLength;
		const IOMemoryDescriptor * md = state->fCopyMD;
		if (!md) md = fMemory;
		ret = md->dmaCommandOperation(kIOMDDMAMap | state->fIterateOnly, &mapArgs, sizeof(mapArgs));
		if (kIOReturnSuccess == ret)
		{
		    state->fLocalMapperPageAlloc = mapArgs.fAlloc;
		    state->fLocalMapperPageCount = mapArgs.fAllocCount;
		    state->fMapContig = true;
		}
		ret = kIOReturnSuccess;
	    }
	}


	if (kIOReturnSuccess == ret)
	    state->fPrepared = true;
    }
    return ret;
}

IOReturn 
IODMACommand::complete(bool invalidateCache, bool synchronize)
{
    IODMACommandInternal * state = fInternalState;
    IOReturn               ret   = kIOReturnSuccess;

    if (fActive < 1)
	return kIOReturnNotReady;

    if (!--fActive)
    {
	if (!state->fCursor)
	{
		IOOptionBits op = kWalkComplete;
		if (synchronize)
			op |= kWalkSyncIn;
		ret = walkAll(op);
	}
    	if (state->fLocalMapperPageAlloc)
    	{
	    if (state->fLocalMapper)
	    {
		fMapper->iovmFreeDMACommand(this, atop_64(state->fLocalMapperPageAlloc), state->fLocalMapperPageCount);
	    }
	    else if (state->fLocalMapperPageCount)
	    {
		fMapper->iovmFree(atop_64(state->fLocalMapperPageAlloc), state->fLocalMapperPageCount);
	    }
	    state->fLocalMapperPageAlloc = 0;
	    state->fLocalMapperPageCount = 0;
	}

	state->fPrepared = false;

	if (IS_NONCOHERENT(fMappingOptions) && invalidateCache)
	{ 
	    IOMemoryDescriptor *poMD = const_cast<IOMemoryDescriptor *>(fMemory);

	    poMD->performOperation(kIOMemoryIncoherentIOFlush, state->fPreparedOffset, state->fPreparedLength);
	}
    }

    return ret;
}

IOReturn
IODMACommand::getPreparedOffsetAndLength(UInt64 * offset, UInt64 * length)
{
    IODMACommandInternal * state = fInternalState;
    if (fActive < 1)
	return (kIOReturnNotReady);

    if (offset)
	*offset = state->fPreparedOffset;
    if (length)
	*length = state->fPreparedLength;

    return (kIOReturnSuccess);
}

IOReturn
IODMACommand::synchronize(IOOptionBits options)
{
    IODMACommandInternal * state = fInternalState;
    IOReturn		   ret   = kIOReturnSuccess;
    IOOptionBits           op;

    if (kIODirectionOutIn == (kIODirectionOutIn & options))
	return kIOReturnBadArgument;

    if (fActive < 1)
	return kIOReturnNotReady;

    op = 0;
    if (kForceDoubleBuffer & options)
    {
	if (state->fDoubleBuffer)
	    return kIOReturnSuccess;
	if (state->fCursor)
	    state->fCursor = false;
	else
	    ret = walkAll(kWalkComplete);

	op |= kWalkPrepare | kWalkPreflight | kWalkDoubleBuffer;
    }
    else if (state->fCursor)
	return kIOReturnSuccess;

    if (kIODirectionIn & options)
	op |= kWalkSyncIn | kWalkSyncAlways;
    else if (kIODirectionOut & options)
	op |= kWalkSyncOut | kWalkSyncAlways;

    ret = walkAll(op);

    return ret;
}

struct IODMACommandTransferContext
{
    void *   buffer;
    UInt64   bufferOffset;
    UInt64   remaining;
    UInt32   op;
};
enum
{
    kIODMACommandTransferOpReadBytes  = 1,
    kIODMACommandTransferOpWriteBytes = 2
};

IOReturn
IODMACommand::transferSegment(void   *reference,
			IODMACommand *target,
			Segment64     segment,
			void         *segments,
			UInt32        segmentIndex)
{
    IODMACommandTransferContext * context = (IODMACommandTransferContext *) reference;
    UInt64   length  = min(segment.fLength, context->remaining);
    addr64_t ioAddr  = segment.fIOVMAddr;
    addr64_t cpuAddr = ioAddr;

    context->remaining -= length;

    while (length)
    {
	UInt64 copyLen = length;
	if ((kMapped == MAPTYPE(target->fMappingOptions))
	    && target->fMapper)
	{
	    cpuAddr = target->fMapper->mapAddr(ioAddr);
	    copyLen = min(copyLen, page_size - (ioAddr & (page_size - 1)));
	    ioAddr += copyLen;
	}

	switch (context->op)
	{
	    case kIODMACommandTransferOpReadBytes:
		copypv(cpuAddr, context->bufferOffset + (addr64_t) context->buffer, copyLen,
				    cppvPsrc | cppvNoRefSrc | cppvFsnk | cppvKmap);
		break;
	    case kIODMACommandTransferOpWriteBytes:
		copypv(context->bufferOffset + (addr64_t) context->buffer, cpuAddr, copyLen,
				cppvPsnk | cppvFsnk | cppvNoRefSrc | cppvNoModSnk | cppvKmap);
		break;
	}
	length                -= copyLen;
	context->bufferOffset += copyLen;
    }
    
    return (context->remaining ? kIOReturnSuccess : kIOReturnOverrun);
}

UInt64
IODMACommand::transfer(IOOptionBits transferOp, UInt64 offset, void * buffer, UInt64 length)
{
    IODMACommandInternal *      state = fInternalState;
    IODMACommandTransferContext context;
    Segment64     		segments[1];
    UInt32                      numSegments = 0-1;

    if (fActive < 1)
        return (0);

    if (offset >= state->fPreparedLength)
        return (0);
    length = min(length, state->fPreparedLength - offset);

    context.buffer       = buffer;
    context.bufferOffset = 0;
    context.remaining    = length;
    context.op           = transferOp;
    (void) genIOVMSegments(kWalkClient, transferSegment, &context, &offset, &segments[0], &numSegments);

    return (length - context.remaining);
}

UInt64
IODMACommand::readBytes(UInt64 offset, void *bytes, UInt64 length)
{
    return (transfer(kIODMACommandTransferOpReadBytes, offset, bytes, length));
}

UInt64
IODMACommand::writeBytes(UInt64 offset, const void *bytes, UInt64 length)
{
    return (transfer(kIODMACommandTransferOpWriteBytes, offset, const_cast<void *>(bytes), length));
}

IOReturn
IODMACommand::genIOVMSegments(UInt64 *offsetP,
			      void   *segmentsP,
			      UInt32 *numSegmentsP)
{
    return (genIOVMSegments(kWalkClient, clientOutputSegment, (void *) fOutSeg,
			    offsetP, segmentsP, numSegmentsP));
}

IOReturn
IODMACommand::genIOVMSegments(uint32_t op,
			      InternalSegmentFunction outSegFunc,
			      void   *reference,
			      UInt64 *offsetP,
			      void   *segmentsP,
			      UInt32 *numSegmentsP)
{
    IODMACommandInternal * internalState = fInternalState;
    IOOptionBits           mdOp = kIOMDWalkSegments;
    IOReturn               ret  = kIOReturnSuccess;

    if (!(kWalkComplete & op) && !fActive)
	return kIOReturnNotReady;

    if (!offsetP || !segmentsP || !numSegmentsP || !*numSegmentsP)
	return kIOReturnBadArgument;

    IOMDDMAWalkSegmentArgs *state =
	(IOMDDMAWalkSegmentArgs *)(void *) fState;

    UInt64 offset    = *offsetP + internalState->fPreparedOffset;
    UInt64 memLength = internalState->fPreparedOffset + internalState->fPreparedLength;

    if (offset >= memLength)
	return kIOReturnOverrun;

    if ((offset == internalState->fPreparedOffset) || (offset != state->fOffset) || internalState->fNewMD) {
	state->fOffset                 = 0;
	state->fIOVMAddr               = 0;
	internalState->fNextRemapPage  = NULL;
	internalState->fNewMD	       = false;
	state->fMapped                 = (IS_MAPPED(fMappingOptions) && fMapper);
	mdOp                           = kIOMDFirstSegment;
    };
	
    UInt64    bypassMask = fBypassMask;
    UInt32    segIndex = 0;
    UInt32    numSegments = *numSegmentsP;
    Segment64 curSeg = { 0, 0 };
    addr64_t  maxPhys;

    if (fNumAddressBits && (fNumAddressBits < 64))
	maxPhys = (1ULL << fNumAddressBits);
    else
	maxPhys = 0;
    maxPhys--;

    while (state->fIOVMAddr || (state->fOffset < memLength))
    {
	// state = next seg
	if (!state->fIOVMAddr) {

	    IOReturn rtn;

	    state->fOffset = offset;
	    state->fLength = memLength - offset;

	    if (internalState->fMapContig && internalState->fLocalMapperPageAlloc)
	    {
		state->fIOVMAddr = internalState->fLocalMapperPageAlloc + offset;
		rtn = kIOReturnSuccess;
#if 0
		{
		    uint64_t checkOffset;
		    IOPhysicalLength segLen;
		    for (checkOffset = 0; checkOffset < state->fLength; )
		    {
			addr64_t phys = const_cast<IOMemoryDescriptor *>(fMemory)->getPhysicalSegment(checkOffset + offset, &segLen, kIOMemoryMapperNone);
			if (fMapper->mapAddr(state->fIOVMAddr + checkOffset) != phys)
			{
			    panic("%llx != %llx:%llx, %llx phys: %llx %llx\n", offset, 
				    state->fIOVMAddr + checkOffset, fMapper->mapAddr(state->fIOVMAddr + checkOffset), state->fLength, 
				    phys, checkOffset);
			}
		        checkOffset += page_size - (phys & page_mask);
		    }
		}
#endif
	    }
	    else
	    {
		const IOMemoryDescriptor * memory =
		    internalState->fCopyMD ? internalState->fCopyMD : fMemory;
		rtn = memory->dmaCommandOperation(mdOp, fState, sizeof(fState));
		mdOp = kIOMDWalkSegments;
	    }

	    if (rtn == kIOReturnSuccess)
	    {
		assert(state->fIOVMAddr);
		assert(state->fLength);
		if ((curSeg.fIOVMAddr + curSeg.fLength) == state->fIOVMAddr) {
		    UInt64 length = state->fLength;
		    offset	    += length;
		    curSeg.fLength  += length;
		    state->fIOVMAddr = 0;
		}
	    }
	    else if (rtn == kIOReturnOverrun)
		state->fIOVMAddr = state->fLength = 0;	// At end
	    else
		return rtn;
	}

	// seg = state, offset = end of seg
	if (!curSeg.fIOVMAddr)
	{
	    UInt64 length = state->fLength;
	    offset	    += length;
	    curSeg.fIOVMAddr = state->fIOVMAddr | bypassMask;
	    curSeg.fLength   = length;
	    state->fIOVMAddr = 0;
	}

        if (!state->fIOVMAddr)
	{
	    if ((kWalkClient & op) && (curSeg.fIOVMAddr + curSeg.fLength - 1) > maxPhys)
	    {
		if (internalState->fCursor)
		{
		    curSeg.fIOVMAddr = 0;
		    ret = kIOReturnMessageTooLarge;
		    break;
		}
		else if (curSeg.fIOVMAddr <= maxPhys)
		{
		    UInt64 remain, newLength;

		    newLength	     = (maxPhys + 1 - curSeg.fIOVMAddr);
		    DEBG("trunc %qx, %qx-> %qx\n", curSeg.fIOVMAddr, curSeg.fLength, newLength);
		    remain	     = curSeg.fLength - newLength;
		    state->fIOVMAddr = newLength + curSeg.fIOVMAddr;
		    curSeg.fLength   = newLength;
		    state->fLength   = remain;
		    offset	    -= remain;
		}
		else 
		{
		    UInt64    addr = curSeg.fIOVMAddr;
		    ppnum_t   addrPage = atop_64(addr);
		    vm_page_t remap = NULL;
		    UInt64    remain, newLength;

		    DEBG("sparse switch %qx, %qx ", addr, curSeg.fLength);

		    remap = internalState->fNextRemapPage;
		    if (remap && (addrPage == vm_page_get_offset(remap)))
		    {
		    }
		    else for (remap = internalState->fCopyPageAlloc; 
				remap && (addrPage != vm_page_get_offset(remap));
				remap = vm_page_get_next(remap))
		    {
		    }

		    if (!remap) panic("no remap page found");

		    curSeg.fIOVMAddr = ptoa_64(vm_page_get_phys_page(remap))
					+ (addr & PAGE_MASK);
		    internalState->fNextRemapPage = vm_page_get_next(remap);

		    newLength		 = PAGE_SIZE - (addr & PAGE_MASK);
		    if (newLength < curSeg.fLength)
		    {
			remain		 = curSeg.fLength - newLength;
			state->fIOVMAddr = addr + newLength;
			curSeg.fLength	 = newLength;
			state->fLength	 = remain;
			offset		-= remain;
		    }
		    DEBG("-> %qx, %qx offset %qx\n", curSeg.fIOVMAddr, curSeg.fLength, offset);
		}
	    }

	    if (curSeg.fLength > fMaxSegmentSize)
	    {
		UInt64 remain = curSeg.fLength - fMaxSegmentSize;

		state->fIOVMAddr = fMaxSegmentSize + curSeg.fIOVMAddr;
		curSeg.fLength   = fMaxSegmentSize;

		state->fLength   = remain;
		offset          -= remain;
	    }

	    if (internalState->fCursor
		&& (0 != (internalState->fSourceAlignMask & curSeg.fIOVMAddr)))
	    {
		curSeg.fIOVMAddr = 0;
		ret = kIOReturnNotAligned;
		break;
	    }

	    if (offset >= memLength)
	    {
		curSeg.fLength   -= (offset - memLength);
		offset = memLength;
		state->fIOVMAddr = state->fLength = 0;	// At end
		break;
	    }
	}

        if (state->fIOVMAddr) {
            if ((segIndex + 1 == numSegments))
                break;

	    ret = (*outSegFunc)(reference, this, curSeg, segmentsP, segIndex++);
            curSeg.fIOVMAddr = 0;
	    if (kIOReturnSuccess != ret)
		break;
        }
    }

    if (curSeg.fIOVMAddr) {
	ret = (*outSegFunc)(reference, this, curSeg, segmentsP, segIndex++);
    }

    if (kIOReturnSuccess == ret)
    {
	state->fOffset = offset;
	*offsetP       = offset - internalState->fPreparedOffset;
	*numSegmentsP  = segIndex;
    }
    return ret;
}

IOReturn 
IODMACommand::clientOutputSegment(
	void *reference, IODMACommand *target,
	Segment64 segment, void *vSegList, UInt32 outSegIndex)
{
    SegmentFunction segmentFunction = (SegmentFunction) reference;
    IOReturn ret = kIOReturnSuccess;

    if (target->fNumAddressBits && (target->fNumAddressBits < 64) 
	&& ((segment.fIOVMAddr + segment.fLength - 1) >> target->fNumAddressBits)
	&& (target->reserved->fLocalMapperPageAlloc || !target->fMapper))
    {
	DEBG("kIOReturnMessageTooLarge(fNumAddressBits) %qx, %qx\n", segment.fIOVMAddr, segment.fLength);
	ret = kIOReturnMessageTooLarge;
    }

    if (!(*segmentFunction)(target, segment, vSegList, outSegIndex))
    {
	DEBG("kIOReturnMessageTooLarge(fOutSeg) %qx, %qx\n", segment.fIOVMAddr, segment.fLength);
	ret = kIOReturnMessageTooLarge;
    }

    return (ret);
}

IOReturn
IODMACommand::genIOVMSegments(SegmentFunction segmentFunction,
				    UInt64   *offsetP,
				    void     *segmentsP,
				    UInt32   *numSegmentsP)
{
    return (genIOVMSegments(kWalkClient, clientOutputSegment, (void *) segmentFunction,
			    offsetP, segmentsP, numSegmentsP));
}

bool 
IODMACommand::OutputHost32(IODMACommand *,
	Segment64 segment, void *vSegList, UInt32 outSegIndex)
{
    Segment32 *base = (Segment32 *) vSegList;
    base[outSegIndex].fIOVMAddr = (UInt32) segment.fIOVMAddr;
    base[outSegIndex].fLength   = (UInt32) segment.fLength;
    return true;
}

bool 
IODMACommand::OutputBig32(IODMACommand *,
	Segment64 segment, void *vSegList, UInt32 outSegIndex)
{
    const UInt offAddr = outSegIndex * sizeof(Segment32);
    const UInt offLen  = offAddr + sizeof(UInt32);
    OSWriteBigInt32(vSegList, offAddr, (UInt32) segment.fIOVMAddr);
    OSWriteBigInt32(vSegList, offLen,  (UInt32) segment.fLength);
    return true;
}

bool
IODMACommand::OutputLittle32(IODMACommand *,
	Segment64 segment, void *vSegList, UInt32 outSegIndex)
{
    const UInt offAddr = outSegIndex * sizeof(Segment32);
    const UInt offLen  = offAddr + sizeof(UInt32);
    OSWriteLittleInt32(vSegList, offAddr, (UInt32) segment.fIOVMAddr);
    OSWriteLittleInt32(vSegList, offLen,  (UInt32) segment.fLength);
    return true;
}

bool
IODMACommand::OutputHost64(IODMACommand *,
	Segment64 segment, void *vSegList, UInt32 outSegIndex)
{
    Segment64 *base = (Segment64 *) vSegList;
    base[outSegIndex] = segment;
    return true;
}

bool
IODMACommand::OutputBig64(IODMACommand *,
	Segment64 segment, void *vSegList, UInt32 outSegIndex)
{
    const UInt offAddr = outSegIndex * sizeof(Segment64);
    const UInt offLen  = offAddr + sizeof(UInt64);
    OSWriteBigInt64(vSegList, offAddr, (UInt64) segment.fIOVMAddr);
    OSWriteBigInt64(vSegList, offLen,  (UInt64) segment.fLength);
    return true;
}

bool
IODMACommand::OutputLittle64(IODMACommand *,
	Segment64 segment, void *vSegList, UInt32 outSegIndex)
{
    const UInt offAddr = outSegIndex * sizeof(Segment64);
    const UInt offLen  = offAddr + sizeof(UInt64);
    OSWriteLittleInt64(vSegList, offAddr, (UInt64) segment.fIOVMAddr);
    OSWriteLittleInt64(vSegList, offLen,  (UInt64) segment.fLength);
    return true;
}


