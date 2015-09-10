// FloatTest.h (Oclgrind)
// Copyright (c) 2015, Moritz Pflanzer
// Imperial College London. All rights reserved.
//
// Based on MemCheckUninitialized plugin by Moritz Pflanzer
//
// This program is provided under a three-clause BSD license. For full
// license terms please see the LICENSE file distributed with this
// source code.

#include "core/common.h"

#include "core/Context.h"
#include "core/Memory.h"
#include "core/WorkItem.h"
#include "core/WorkGroup.h"
#include "core/Kernel.h"
#include "core/KernelInvocation.h"

#include "llvm/IR/Instructions.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Argument.h"
#include "llvm/IR/Type.h"

#include "llvm/ADT/APFloat.h"
#include "llvm/IR/Type.h"
#include "llvm/Support/Casting.h"
#include <cmath>

#include "FloatTest.h"
#include <mutex>

using namespace oclgrind;
using namespace std;

//void MemCheckUninitialized::memoryAllocated(const Memory *memory, size_t address,
//                                 size_t size, cl_mem_flags flags,
//                                 const uint8_t *initData)
//{
//    cout << "Memory: " << memory << ", address: " << hex << address << dec << ", size: " << size << endl;
//}

// Multiple mutexes to mitigate risk of unnecessary synchronisation in atomics
#define NUM_ATOMIC_MUTEXES 64 // Must be power of two
static std::mutex atomicShadowMutex[NUM_ATOMIC_MUTEXES];
#define ATOMIC_MUTEX(offset) \
  atomicShadowMutex[(((offset)>>2) & (NUM_ATOMIC_MUTEXES-1))]

THREAD_LOCAL ShadowContext::WorkSpace ShadowContext::m_workSpace = {NULL, NULL, NULL, 0};

FloatTest::FloatTest(const Context *context)
 : Plugin(context), shadowContext(sizeof(size_t)==8 ? 32 : 16)
{
    shadowContext.createMemoryPool();
}

FloatTest::~FloatTest()
{
    shadowContext.destroyMemoryPool();
}

void FloatTest::allocAndStoreShadowMemory(unsigned addrSpace, size_t address, Interval* inter,
        const WorkItem *workItem, const WorkGroup *workGroup, bool unchecked)
{
    if(addrSpace == AddrSpaceConstant)
    {
    	cout << "got constant" << endl;
        //TODO: Eventually store value
        return;
    }

    ShadowMemory *memory = getShadowMemory(addrSpace, workItem, workGroup);
    memory->allocate(address);
    storeShadowMemory(addrSpace, address, inter, workItem, workGroup, unchecked);
}

// this won't be used
/*
bool FloatTest::checkAllOperandsDefined(const WorkItem *workItem, const llvm::Instruction *I)
{
    for(llvm::Instruction::const_op_iterator OI = I->op_begin(); OI != I->op_end(); ++OI)
    {

        if(!ShadowContext::isCleanValue(shadowContext.getValue(workItem, OI->get())))
        {
#ifdef DUMP_SHADOW
            OI->get()->dump();
            cout << "Shadow value: " << shadowContext.getValue(workItem, OI->get()) << endl;
#endif
            logUninitializedCF();
#ifdef DUMP_SHADOW
            shadowContext.dump(workItem);
#endif
            return false;
        }

    }

    return true;
}
*/

void FloatTest::checkStructMemcpy(const WorkItem *workItem, const llvm::Value *src)
{
    const llvm::PointerType *srcPtrTy = llvm::dyn_cast<llvm::PointerType>(src->getType());
    const llvm::StructType *structTy = llvm::dyn_cast<llvm::StructType>(srcPtrTy->getElementType());
    size_t srcAddr = workItem->getOperand(src).getPointer();
    unsigned srcAddrSpace = srcPtrTy->getPointerAddressSpace();

    ShadowMemory *shadowMemory;

    switch(srcAddrSpace)
    {
        case AddrSpacePrivate:
        {
            shadowMemory = shadowContext.getShadowWorkItem(workItem)->getPrivateMemory();
            break;
        }
        case AddrSpaceLocal:
        {
            shadowMemory = shadowContext.getShadowWorkGroup(workItem->getWorkGroup())->getLocalMemory();
            break;
        }
        case AddrSpaceConstant:
            //TODO: Constants should always be clean?!
            return;
        case AddrSpaceGlobal:
            shadowMemory = shadowContext.getGlobalMemory();
            break;
        default:
            FATAL_ERROR("Unsupported addressspace %d", srcAddrSpace);
    }

    /*
    if(!ShadowContext::isCleanStruct(shadowMemory, srcAddr, structTy))
    {
        logUninitializedWrite(srcAddrSpace, srcAddr);
    }
    */
}

//not used
/*
void FloatTest::copyShadowMemory(unsigned dstAddrSpace, size_t dst, unsigned srcAddrSpace, size_t src, unsigned size, const WorkItem *workItem, const WorkGroup *workGroup, bool unchecked)
{
    copyShadowMemoryStrided(dstAddrSpace, dst, srcAddrSpace, src, 1, 1, size, workItem, workGroup, unchecked);
}

void FloatTest::copyShadowMemoryStrided(unsigned dstAddrSpace, size_t dst, unsigned srcAddrSpace, size_t src, size_t num, size_t stride, unsigned size, const WorkItem *workItem, const WorkGroup *workGroup, bool unchecked)
{
    TypedValue v = {
        size,
        1,
        new unsigned char[size]
    };

    for (unsigned i = 0; i < num; i++)
    {
        loadShadowMemory(srcAddrSpace, src, v, workItem, workGroup);
        storeShadowMemory(dstAddrSpace, dst, v, workItem, workGroup, unchecked);
        src += stride * size;
        dst += stride * size;
    }

    delete[] v.data;
}
*/

std::string FloatTest::extractUnmangledName(const std::string fullname)
{
    // Extract unmangled name
    if(fullname.compare(0,2, "_Z") == 0)
    {
        int len = atoi(fullname.c_str() + 2);
        int start = fullname.find_first_not_of("0123456789", 2);
        return fullname.substr(start, len);
    }
    else
    {
        return fullname;
    }
}

ShadowMemory* FloatTest::getShadowMemory(unsigned addrSpace,
        const WorkItem *workItem, const WorkGroup *workGroup) const
{
    switch(addrSpace)
    {
        case AddrSpacePrivate:
        {
            if(!workItem)
            {
                FATAL_ERROR("Work item needed to access private memory!");
            }

            return shadowContext.getShadowWorkItem(workItem)->getPrivateMemory();
        }
        case AddrSpaceLocal:
        {
            if(!workGroup)
            {
                if(!workItem)
                {
                    FATAL_ERROR("Work item or work group needed to access local memory!");
                }

                workGroup = workItem->getWorkGroup();
            }

            return shadowContext.getShadowWorkGroup(workGroup)->getLocalMemory();
        }
        //case AddrSpaceConstant:
        //    break;
        case AddrSpaceGlobal:
            return shadowContext.getGlobalMemory();
        default:
            FATAL_ERROR("Unsupported addressspace %d", addrSpace);
    }
}


// THIS IS NOT USED ANYWHERE, COMMENTED OUT FOR NOW
/*
bool FloatTest::handleBuiltinFunction(const WorkItem *workItem, string name,
                                                  const llvm::CallInst *CI, const TypedValue result)
{
    name = extractUnmangledName(name);
    ShadowValues *shadowValues = shadowContext.getShadowWorkItem(workItem)->getValues();

    if(name == "async_work_group_copy" ||
       name == "async_work_group_strided_copy")
    {
        int arg = 0;

        // Get src/dest addresses
        const llvm::Value *dstOp = CI->getArgOperand(arg++);
        const llvm::Value *srcOp = CI->getArgOperand(arg++);
        size_t dst = workItem->getOperand(dstOp).getPointer();
        size_t src = workItem->getOperand(srcOp).getPointer();

        // Get size of copy
        unsigned elemSize = getTypeSize(dstOp->getType()->getPointerElementType());

        const llvm::Value *numOp = CI->getArgOperand(arg++);
        uint64_t num = workItem->getOperand(numOp).getUInt();
        TypedValue numShadow = shadowContext.getValue(workItem, numOp);

        if(!ShadowContext::isCleanValue(numShadow))
        {
            logUninitializedIndex();
        }

        // Get stride
        size_t stride = 1;

        if(name == "async_work_group_strided_copy")
        {
            const llvm::Value *strideOp = CI->getArgOperand(arg++);
            stride = workItem->getOperand(strideOp).getUInt();
            TypedValue strideShadow = shadowContext.getValue(workItem, strideOp);

            if(!ShadowContext::isCleanValue(strideShadow))
            {
                logUninitializedIndex();
            }
        }

        const llvm::Value *eventOp = CI->getArgOperand(arg++);
        TypedValue eventShadow = shadowContext.getValue(workItem, eventOp);

        // Get type of copy
        AddressSpace dstAddrSpace = AddrSpaceLocal;
        AddressSpace srcAddrSpace = AddrSpaceLocal;

        if(dstOp->getType()->getPointerAddressSpace() == AddrSpaceLocal)
        {
            srcAddrSpace = AddrSpaceGlobal;
        }
        else
        {
            dstAddrSpace = AddrSpaceGlobal;
        }

        copyShadowMemoryStrided(dstAddrSpace, dst, srcAddrSpace, src, num, stride, elemSize, workItem);
        shadowValues->setValue(CI, eventShadow);

        // Check shadow of src address
        TypedValue srcShadow = shadowContext.getValue(workItem, srcOp);

        if(!ShadowContext::isCleanValue(srcShadow))
        {
            logUninitializedAddress(srcAddrSpace, src, false);
        }

        // Check shadow of dst address
        TypedValue dstShadow = shadowContext.getValue(workItem, dstOp);

        if(!ShadowContext::isCleanValue(dstShadow))
        {
            logUninitializedAddress(dstAddrSpace, dst);
        }
        
        return true;
    }
    else if(name == "wait_group_events")
    {
        const llvm::Value *Addr = CI->getArgOperand(1);
        const llvm::Value *Num = CI->getArgOperand(0);
        uint64_t num = workItem->getOperand(Num).getUInt();
        size_t address = workItem->getOperand(Addr).getPointer();

        TypedValue numShadow = shadowContext.getValue(workItem, Num);
        TypedValue eventShadow = {
            sizeof(size_t),
            1,
            new unsigned char[sizeof(size_t)]
        };

        // Check shadow for the number of events
        if(!ShadowContext::isCleanValue(numShadow))
        {
            logUninitializedCF();
        }

        for(unsigned i = 0; i < num; ++i)
        {
            loadShadowMemory(AddrSpacePrivate, address, eventShadow, workItem);

            if(!ShadowContext::isCleanValue(eventShadow))
            {
                logUninitializedCF();
                delete[] eventShadow.data;
                return true;
            }

            address += sizeof(size_t);
        }

        delete[] eventShadow.data;

        // Check shadow of address
        TypedValue addrShadow = shadowContext.getValue(workItem, Addr);

        if(!ShadowContext::isCleanValue(addrShadow))
        {
            logUninitializedAddress(AddrSpacePrivate, address, false);
        }

        return true;
    }
    else if(name.compare(0, 6, "atomic") == 0)
    {
        if(name.compare(6, string::npos, "cmpxchg") == 0)
        {
            const llvm::Value *Addr = CI->getArgOperand(0);
            unsigned addrSpace = Addr->getType()->getPointerAddressSpace();
            size_t address = workItem->getOperand(Addr).getPointer();
            uint32_t cmp = workItem->getOperand(CI->getArgOperand(1)).getUInt();
            uint32_t old = workItem->getOperand(CI).getUInt();
            TypedValue argShadow = shadowContext.getValue(workItem, CI->getArgOperand(2));
            TypedValue cmpShadow = shadowContext.getValue(workItem, CI->getArgOperand(1));
            TypedValue oldShadow = {
                4,
                1,
                shadowContext.getMemoryPool()->alloc(4)
            };

            // Check shadow of the condition
            if(!ShadowContext::isCleanValue(cmpShadow))
            {
                logUninitializedCF();
            }

            // Perform cmpxchg
            if(addrSpace == AddrSpaceGlobal)
            {
                shadowContext.getGlobalMemory()->lock(address);
            }

            loadShadowMemory(addrSpace, address, oldShadow, workItem);

            if(old == cmp)
            {
                storeShadowMemory(addrSpace, address, argShadow, workItem);
            }

            if(addrSpace == AddrSpaceGlobal)
            {
                shadowContext.getGlobalMemory()->unlock(address);
            }

            shadowValues->setValue(CI, oldShadow);

            // Check shadow of address
            TypedValue addrShadow = shadowContext.getValue(workItem, Addr);

            if(!ShadowContext::isCleanValue(addrShadow))
            {
                logUninitializedAddress(addrSpace, address);
            }

            return true;
        }
        else
        {
            SimpleOrAtomic(workItem, CI);
            return true;
        }
    }
    else if(name == "fract" ||
            name == "modf" ||
            name == "sincos")
    {
        const llvm::Value *Addr = CI->getArgOperand(1);
        unsigned addrSpace = Addr->getType()->getPointerAddressSpace();
        size_t iptr = workItem->getOperand(Addr).getPointer();
        TypedValue argShadow = shadowContext.getValue(workItem, CI->getArgOperand(0));
        TypedValue newElemShadow;
        TypedValue newShadow = shadowContext.getMemoryPool()->clone(result);

        for(unsigned i = 0; i < result.num; ++i)
        {
            if(!ShadowContext::isCleanValue(argShadow, i))
            {
                newElemShadow = ShadowContext::getPoisonedValue(result.size);
            }
            else
            {
                newElemShadow = ShadowContext::getCleanValue(result.size);
            }

            memcpy(newShadow.data, newElemShadow.data, result.size);
        }

        storeShadowMemory(addrSpace, iptr, newShadow);
        shadowValues->setValue(CI, newShadow);

        // Check shadow of address
        TypedValue addrShadow = shadowContext.getValue(workItem, Addr);

        if(!ShadowContext::isCleanValue(addrShadow))
        {
            logUninitializedAddress(addrSpace, iptr);
        }

        return true;
    }
    else if(name == "frexp" ||
            name == "lgamma_r")
    {
        const llvm::Value *Addr = CI->getArgOperand(1);
        unsigned addrSpace = Addr->getType()->getPointerAddressSpace();
        size_t iptr = workItem->getOperand(Addr).getPointer();
        TypedValue argShadow = shadowContext.getValue(workItem, CI->getArgOperand(0));
        TypedValue newElemShadow;
        TypedValue newElemIntShadow;
        TypedValue newShadow = shadowContext.getMemoryPool()->clone(result);
        TypedValue newIntShadow = {
            newShadow.size,
            newShadow.num,
            shadowContext.getMemoryPool()->alloc(4)
        };

        for(unsigned i = 0; i < result.num; ++i)
        {
            if(!ShadowContext::isCleanValue(argShadow, i))
            {
                newElemShadow = ShadowContext::getPoisonedValue(result.size);
                newElemIntShadow = ShadowContext::getPoisonedValue(4);
            }
            else
            {
                newElemShadow = ShadowContext::getCleanValue(result.size);
                newElemIntShadow = ShadowContext::getCleanValue(4);
            }

            memcpy(newIntShadow.data, newElemIntShadow.data, 4);
            memcpy(newShadow.data, newElemShadow.data, result.size);
        }

        storeShadowMemory(addrSpace, iptr, newIntShadow);
        shadowValues->setValue(CI, newShadow);

        // Check shadow of address
        TypedValue addrShadow = shadowContext.getValue(workItem, Addr);

        if(!ShadowContext::isCleanValue(addrShadow))
        {
            logUninitializedAddress(addrSpace, iptr);
        }

        return true;
    }
    else if(name == "remquo")
    {
        const llvm::Value *Addr = CI->getArgOperand(2);
        unsigned addrSpace = Addr->getType()->getPointerAddressSpace();
        size_t iptr = workItem->getOperand(Addr).getPointer();
        TypedValue arg0Shadow = shadowContext.getValue(workItem, CI->getArgOperand(0));
        TypedValue arg1Shadow = shadowContext.getValue(workItem, CI->getArgOperand(1));
        TypedValue newElemShadow;
        TypedValue newShadow = shadowContext.getMemoryPool()->clone(result);

        for(unsigned i = 0; i < result.num; ++i)
        {
            if(!ShadowContext::isCleanValue(arg0Shadow, i) || !ShadowContext::isCleanValue(arg1Shadow, i))
            {
                newElemShadow = ShadowContext::getPoisonedValue(result.size);
            }
            else
            {
                newElemShadow = ShadowContext::getCleanValue(result.size);
            }

            storeShadowMemory(addrSpace, iptr + i*4, newElemShadow);
            memcpy(newShadow.data, newElemShadow.data, result.size);
        }

        shadowValues->setValue(CI, newShadow);

        // Check shadow of address
        TypedValue addrShadow = shadowContext.getValue(workItem, Addr);

        if(!ShadowContext::isCleanValue(addrShadow))
        {
            logUninitializedAddress(addrSpace, iptr);
        }

        return true;
    }
    else if(name == "shuffle")
    {
        TypedValue mask = workItem->getOperand(CI->getArgOperand(1));
        TypedValue maskShadow = shadowContext.getValue(workItem, CI->getArgOperand(1));
        TypedValue shadow = shadowContext.getValue(workItem, CI->getArgOperand(0));
        TypedValue newShadow = shadowContext.getMemoryPool()->clone(result);

        for(unsigned i = 0; i < newShadow.num; ++i)
        {
            if(!ShadowContext::isCleanValue(maskShadow, i))
            {
                TypedValue v = ShadowContext::getPoisonedValue(newShadow.size);
                memcpy(newShadow.data + i*newShadow.size, v.data, newShadow.size);
            }
            else
            {
                size_t srcOffset = mask.getUInt(i) * shadow.size;
                memcpy(newShadow.data + i*newShadow.size, shadow.data + srcOffset, newShadow.size);
            }
        }

        shadowValues->setValue(CI, newShadow);
        return true;
    }
    else if(name == "shuffle2")
    {
        TypedValue mask = workItem->getOperand(CI->getArgOperand(2));
        TypedValue maskShadow = shadowContext.getValue(workItem, CI->getArgOperand(2));
        TypedValue shadow[] = {shadowContext.getValue(workItem, CI->getArgOperand(0)),
                               shadowContext.getValue(workItem, CI->getArgOperand(1))};
        TypedValue newShadow = shadowContext.getMemoryPool()->clone(result);

        for (unsigned i = 0; i < newShadow.num; ++i)
        {
            uint64_t m = 1;

            if(CI->getArgOperand(0)->getType()->isVectorTy())
            {
                m = CI->getArgOperand(0)->getType()->getVectorNumElements();
            }

            uint64_t src = 0;
            uint64_t index = mask.getUInt(i);

            if(index >= m)
            {
                index -= m;
                src = 1;
            }

            if(!ShadowContext::isCleanValue(maskShadow, i))
            {
                TypedValue v = ShadowContext::getPoisonedValue(newShadow.size);
                memcpy(newShadow.data + i*newShadow.size, v.data, newShadow.size);
            }
            else
            {
                size_t srcOffset = index * shadow[src].size;
                memcpy(newShadow.data + i*newShadow.size, shadow[src].data + srcOffset, newShadow.size);
            }
        }

        shadowValues->setValue(CI, newShadow);
        return true;
    }
    else if(name == "any")
    {
        const llvm::Value *argOp = CI->getArgOperand(0);
        TypedValue shadow = shadowContext.getValue(workItem, argOp);

        unsigned num = 1;
        if(argOp->getType()->isVectorTy())
        {
            num = argOp->getType()->getVectorNumElements();
        }

        for(unsigned i = 0; i < num; ++i)
        {
            if(ShadowContext::isCleanValue(shadow, i))
            {
                shadowValues->setValue(CI, ShadowContext::getCleanValue(result.size));
                return true;
            }
        }

        shadowValues->setValue(CI, ShadowContext::getPoisonedValue(result.size));
        return true;
    }
    else if(name == "select")
    {
        TypedValue newShadow = shadowContext.getMemoryPool()->clone(result);
        TypedValue shadow[] = {shadowContext.getValue(workItem, CI->getArgOperand(0)),
                               shadowContext.getValue(workItem, CI->getArgOperand(1))};
        TypedValue selectShadow = shadowContext.getValue(workItem, CI->getArgOperand(2));

        for(unsigned i = 0; i < newShadow.num; ++i)
        {
            int64_t c = workItem->getOperand(CI->getArgOperand(2)).getSInt(i);
            uint64_t src = ((newShadow.num > 1) ? c & INT64_MIN : c) ? 1 : 0;

            if(!ShadowContext::isCleanValue(selectShadow, i))
            {
                TypedValue v = ShadowContext::getPoisonedValue(newShadow.size);
                memcpy(newShadow.data + i*newShadow.size, v.data, newShadow.size);
            }
            else
            {
                size_t srcOffset = i * shadow[src].size;
                memcpy(newShadow.data + i*newShadow.size, shadow[src].data + srcOffset, newShadow.size);
            }
        }

        shadowValues->setValue(CI, newShadow);
        return true;
    }
    else if(name.compare(0, 10, "vload_half") == 0 ||
            name.compare(0, 11, "vloada_half") == 0)
    {
        const llvm::Value *BaseOp = CI->getArgOperand(1);
        const llvm::Value *OffsetOp = CI->getArgOperand(0);
        size_t base = workItem->getOperand(BaseOp).getPointer();
        unsigned int addressSpace = BaseOp->getType()->getPointerAddressSpace();
        uint64_t offset = workItem->getOperand(OffsetOp).getUInt();

        size_t address;

        if(name.compare(0, 6, "vloada") == 0 && result.num == 3)
        {
            address = base + offset * sizeof(cl_half) * 4;
        }
        else
        {
            address = base + offset * sizeof(cl_half) * result.num;
        }

        TypedValue halfShadow = {
            sizeof(cl_half),
            result.num,
            shadowContext.getMemoryPool()->alloc(2 * result.num)
        };
        TypedValue newShadow = shadowContext.getMemoryPool()->clone(result);

        loadShadowMemory(addressSpace, address, halfShadow, workItem);

        TypedValue pv = ShadowContext::getPoisonedValue(newShadow.size);
        TypedValue cv = ShadowContext::getCleanValue(newShadow.size);

        // Convert to float shadows
        for(unsigned i = 0; i < newShadow.num; ++i)
        {
            if(!ShadowContext::isCleanValue(halfShadow, i))
            {
                memcpy(newShadow.data + i*newShadow.size, pv.data, newShadow.size);
            }
            else
            {
                memcpy(newShadow.data + i*newShadow.size, cv.data, newShadow.size);
            }
        }

        shadowValues->setValue(CI, newShadow);

        // Check shadow of address
        TypedValue baseShadow = shadowContext.getValue(workItem, BaseOp);
        TypedValue offsetShadow = shadowContext.getValue(workItem, OffsetOp);

        if(!ShadowContext::isCleanValue(baseShadow) ||
           !ShadowContext::isCleanValue(offsetShadow))
        {
            logUninitializedAddress(addressSpace, address, false);
        }

        return true;
    }
    else if(name.compare(0, 11, "vstore_half") == 0 ||
            name.compare(0, 12, "vstorea_half") == 0)
    {
        const llvm::Value *value = CI->getArgOperand(0);
        unsigned size = getTypeSize(value->getType());

        if(isVector3(value))
        {
            // 3-element vectors are same size as 4-element vectors,
            // but vstore address offset shouldn't use this.
            size = (size / 4) * 3;
        }

        const llvm::Value *BaseOp = CI->getArgOperand(2);
        const llvm::Value *OffsetOp = CI->getArgOperand(1);
        size_t base = workItem->getOperand(BaseOp).getPointer();
        unsigned int addressSpace = BaseOp->getType()->getPointerAddressSpace();
        uint64_t offset = workItem->getOperand(OffsetOp).getUInt();

        // Convert to halfs
        TypedValue shadow = shadowContext.getValue(workItem, value);
        unsigned num = size / sizeof(float);
        size = num * sizeof(cl_half);
        TypedValue halfShadow = {
            sizeof(cl_half),
            num,
            shadowContext.getMemoryPool()->alloc(2 * num)
        };

        TypedValue pv = ShadowContext::getPoisonedValue(halfShadow.size);
        TypedValue cv = ShadowContext::getCleanValue(halfShadow.size);

        for(unsigned i = 0; i < num; i++)
        {
            if(!ShadowContext::isCleanValue(shadow, i))
            {
                memcpy(halfShadow.data + i*halfShadow.size, pv.data, halfShadow.size);
            }
            else
            {
                memcpy(halfShadow.data + i*halfShadow.size, cv.data, halfShadow.size);
            }
        }

        size_t address;
        if(name.compare(0, 7, "vstorea") == 0 && num == 3)
        {
            address = base + offset * sizeof(cl_half) * 4;
        }
        else
        {
            address = base + offset * sizeof(cl_half) * num;
        }

        storeShadowMemory(addressSpace, address, halfShadow, workItem);

        // Check shadow of address
        TypedValue baseShadow = shadowContext.getValue(workItem, BaseOp);
        TypedValue offsetShadow = shadowContext.getValue(workItem, OffsetOp);


        if(!ShadowContext::isCleanValue(baseShadow) ||
           !ShadowContext::isCleanValue(offsetShadow))
        {
            logUninitializedAddress(addressSpace, address);
        }

        return true;
    }
    else if(name.compare(0, 5, "vload") == 0)
    {
        TypedValue newShadow = shadowContext.getMemoryPool()->clone(result);
        const llvm::Value *BaseOp = CI->getArgOperand(1);
        const llvm::Value *OffsetOp = CI->getArgOperand(0);
        unsigned int addressSpace = BaseOp->getType()->getPointerAddressSpace();
        size_t base = workItem->getOperand(BaseOp).getPointer();
        uint64_t offset = workItem->getOperand(OffsetOp).getUInt();

        size_t size = newShadow.size*newShadow.num;
        size_t address = base + offset*size;
        loadShadowMemory(addressSpace, address, newShadow, workItem);

        shadowValues->setValue(CI, newShadow);

        // Check shadow of address
        TypedValue baseShadow = shadowContext.getValue(workItem, BaseOp);
        TypedValue offsetShadow = shadowContext.getValue(workItem, OffsetOp);


        if(!ShadowContext::isCleanValue(baseShadow) ||
           !ShadowContext::isCleanValue(offsetShadow))
        {
            logUninitializedAddress(addressSpace, address, false);
        }


        return true;
    }
    else if(name.compare(0, 6, "vstore") == 0)
    {
        const llvm::Value *value = CI->getArgOperand(0);
        unsigned size = getTypeSize(value->getType());

        if(isVector3(value))
        {
            // 3-element vectors are same size as 4-element vectors,
            // but vstore address offset shouldn't use this.
            size = (size/4) * 3;
        }

        const llvm::Value *BaseOp = CI->getArgOperand(2);
        const llvm::Value *OffsetOp = CI->getArgOperand(1);
        unsigned int addressSpace = BaseOp->getType()->getPointerAddressSpace();
        size_t base = workItem->getOperand(BaseOp).getPointer();
        uint64_t offset = workItem->getOperand(OffsetOp).getUInt();

        size_t address = base + offset*size;
        TypedValue shadow = shadowContext.getValue(workItem, value);
        storeShadowMemory(addressSpace, address, shadow, workItem);

        // Check shadow of address
        TypedValue baseShadow = shadowContext.getValue(workItem, BaseOp);
        TypedValue offsetShadow = shadowContext.getValue(workItem, OffsetOp);


        if(!ShadowContext::isCleanValue(baseShadow) ||
           !ShadowContext::isCleanValue(offsetShadow))
        {
            logUninitializedAddress(addressSpace, address);
        }


        return true;
    }

    return false;
}
*/

// not used
/*
void FloatTest::handleIntrinsicInstruction(const WorkItem *workItem, const llvm::IntrinsicInst *I)
{
    switch (I->getIntrinsicID())
    {
        case llvm::Intrinsic::fmuladd:
        {
        	// TODO : handle fmuladd
            //SimpleOr(workItem, I);
            break;
        }
        case llvm::Intrinsic::memcpy:
        {
            const llvm::MemCpyInst *memcpyInst = (const llvm::MemCpyInst*)I;
            const llvm::Value *dstOp = memcpyInst->getDest();
            const llvm::Value *srcOp = memcpyInst->getSource();
            size_t dst = workItem->getOperand(dstOp).getPointer();
            size_t src = workItem->getOperand(srcOp).getPointer();
            size_t size = workItem->getOperand(memcpyInst->getLength()).getUInt();
            unsigned dstAddrSpace = memcpyInst->getDestAddressSpace();
            unsigned srcAddrSpace = memcpyInst->getSourceAddressSpace();
            const llvm::PointerType *srcPtrTy = llvm::dyn_cast<llvm::PointerType>(memcpyInst->getSource()->getType());

            if(dstAddrSpace != AddrSpacePrivate && srcPtrTy->getElementType()->isStructTy())
            {
                checkStructMemcpy(workItem, memcpyInst->getSource());
            }

            copyShadowMemory(dstAddrSpace, dst, srcAddrSpace, src, size, workItem, NULL, true);

            // Check shadow of src address
            TypedValue srcShadow = shadowContext.getValue(workItem, srcOp);


            //if(!ShadowContext::isCleanValue(srcShadow))
            //{
            //    logUninitializedAddress(srcAddrSpace, src, false);
            //}


            // Check shadow of dst address
            TypedValue dstShadow = shadowContext.getValue(workItem, dstOp);


            //if(!ShadowContext::isCleanValue(dstShadow))
            //{
            //    logUninitializedAddress(dstAddrSpace, dst);
            //}

            break;
        }
        case llvm::Intrinsic::memset:
        {
            const llvm::MemSetInst *memsetInst = (const llvm::MemSetInst*)I;
            const llvm::Value *Addr = memsetInst->getDest();
            size_t dst = workItem->getOperand(Addr).getPointer();
            unsigned size = workItem->getOperand(memsetInst->getLength()).getUInt();
            unsigned addrSpace = memsetInst->getDestAddressSpace();

            TypedValue shadowValue = {
                size,
                1,
                new unsigned char[size]
            };

            memset(shadowValue.data, shadowContext.getValue(workItem, memsetInst->getArgOperand(1)).getUInt(), size);
            storeShadowMemory(addrSpace, dst, shadowValue, workItem, NULL, true);

            delete[] shadowValue.data;

            // Check shadow of address
            TypedValue addrShadow = shadowContext.getValue(workItem, Addr);


            //if(!ShadowContext::isCleanValue(addrShadow))
            //{
            //    logUninitializedAddress(addrSpace, dst);
            //}

            break;
        }
        case llvm::Intrinsic::dbg_declare:
            //Do nothing
            break;
        case llvm::Intrinsic::dbg_value:
            //Do nothing
            break;
        case llvm::Intrinsic::lifetime_end:
            //Do nothing
            break;
        case llvm::Intrinsic::lifetime_start:
            //Do nothing
            break;
        default:
            FATAL_ERROR("Unsupported intrinsic %s", llvm::Intrinsic::getName(I->getIntrinsicID()).c_str());
    }
}
*/

void FloatTest::hostMemoryStore(const Memory *memory,
                             size_t address, size_t size,
                             const uint8_t *storeData)
{
    if(memory->getAddressSpace() == AddrSpaceGlobal)
    {
    	//TODO : handle floats here

        //TypedValue v = ShadowContext::getCleanValue(size);
        //allocAndStoreShadowMemory(AddrSpaceGlobal, address, v);
    }
}

// handles FAdd, FSub, FMul, FDiv
void FloatTest::simpleFloatInstruction(const WorkItem *workItem, const llvm::Instruction *instruction){

	llvm::Value* lhs = instruction->getOperand(0);
	llvm::Value* rhs = instruction->getOperand(1);

	ShadowValues *shadowValues = shadowContext.getShadowWorkItem(workItem)->getValues();

	Interval lhsVal = *(shadowValues->getValue(lhs));
	Interval rhsVal = *(shadowValues->getValue(rhs));

	Interval res;
	switch(instruction->getOpcode()){
	case llvm::Instruction::FAdd:
		res = lhsVal + rhsVal;
		break;
	case llvm::Instruction::Sub:
		res = lhsVal - rhsVal;
		break;
	case llvm::Instruction::FMul:
		res = lhsVal * rhsVal;
		break;
	case llvm::Instruction::FDiv:
		res = lhsVal / rhsVal;
		break;
	default:
		assert(false && "unsupported instruction");
		break;
	}

	cout << "result = " << res.lower() << " " << res.upper() << endl;

	Interval* shadowVal = new Interval(res);
	shadowValues->setValue(instruction, shadowVal);

}


// Do I even need to handle this?
// Check if the result agrees with the result from shadowMemory
void FloatTest::handleCmpInstruction(const WorkItem *workItem, const llvm::Instruction *instruction, const TypedValue& result){

	llvm::FCmpInst *cmpInst = ((llvm::FCmpInst*)instruction);
	llvm::FCmpInst::Predicate predicate = cmpInst->getPredicate();
	llvm::Value *lhs = cmpInst->getOperand(0);
	llvm::Value *rhs = cmpInst->getOperand(1);

    ShadowValues *shadowValues = shadowContext.getShadowWorkItem(workItem)->getValues();

    Interval lhsVal = *(shadowValues->getValue(lhs));
    Interval rhsVal = *(shadowValues->getValue(rhs));

    cout << "lhs : " << lhsVal.lower() << " " << lhsVal.upper() << endl;
    cout << "rhs : " << rhsVal.lower() << " " << rhsVal.upper() << endl;

    bool actual = result.getSInt();

	switch(predicate){
	case llvm::FCmpInst::Predicate::FCMP_FALSE:///< 0 0 0 0    Always false (always folded)
		assert(actual == false && "FCMP_FALSE");
		break;
	case llvm::FCmpInst::Predicate::FCMP_OEQ:  ///< 0 0 0 1    True if ordered and equal
		//assert(!isnan(lhsVal) && !isnan(lhsVal) && (lhsVal == rhsVal) == actual && "FCMP_OEQ");
		break;
	case llvm::FCmpInst::Predicate::FCMP_OGT:  ///< 0 0 1 0    True if ordered and greater than
		//assert(!isnan(lhsVal) && !isnan(lhsVal) && (lhsVal > rhsVal) == actual && "FCMP_OGT");
		break;
	case llvm::FCmpInst::Predicate::FCMP_OGE:  ///< 0 0 1 1    True if ordered and greater than or equal
		//assert(!isnan(lhsVal) && !isnan(lhsVal) && (lhsVal >= rhsVal) == actual && "FCMP_OGE");
		break;
	case llvm::FCmpInst::Predicate::FCMP_OLT:  ///< 0 1 0 0    True if ordered and less than
		//assert(!isnan(lhsVal) && !isnan(lhsVal) && (lhsVal < rhsVal) == actual && "FCMP_OLT");
		break;
	case llvm::FCmpInst::Predicate::FCMP_OLE:  ///< 0 1 0 1    True if ordered and less than or equal
		//assert(!isnan(lhsVal) && !isnan(lhsVal) && (lhsVal <= rhsVal) == actual && "FCMP_OLE");
		break;
	case llvm::FCmpInst::Predicate::FCMP_ONE:  ///< 0 1 1 0    True if ordered and operands are unequal
		//assert(!isnan(lhsVal) && !isnan(lhsVal) && (lhsVal != rhsVal) == actual && "FCMP_ONE");
		break;
	case llvm::FCmpInst::Predicate::FCMP_ORD:  ///< 0 1 1 1    True if ordered (no nans)
		//assert(!isnan(lhsVal) && !isnan(lhsVal) && "FCMP_ORD");
		break;
	case llvm::FCmpInst::Predicate::FCMP_UNO:  ///< 1 0 0 0    True if unordered: isnan(X) | isnan(Y)
		//assert(isnan(lhsVal) || isnan(rhsVal));
		break;
	case llvm::FCmpInst::Predicate::FCMP_UEQ:  ///< 1 0 0 1    True if unordered or equal
		//assert((isnan(lhsVal) || isnan(rhsVal) || lhsVal == rhsVal) == actual && "FCMP_UEQ");
		break;
	case llvm::FCmpInst::Predicate::FCMP_UGT:  ///< 1 0 1 0    True if unordered or greater than
		//assert((isnan(lhsVal) || isnan(rhsVal) || lhsVal > rhsVal) == actual && "FCMP_UGT");
		break;
	case llvm::FCmpInst::Predicate::FCMP_UGE:  ///< 1 0 1 1    True if unordered, greater than, or equal
		//assert((isnan(lhsVal) || isnan(rhsVal) || lhsVal >= rhsVal) == actual && "FCMP_UGE");
		break;
	case llvm::FCmpInst::Predicate::FCMP_ULT:  ///< 1 1 0 0    True if unordered or less than
		//assert((isnan(lhsVal) || isnan(rhsVal) || lhsVal < rhsVal) == actual && "FCMP_ULT");
		break;
	case llvm::FCmpInst::Predicate::FCMP_ULE:  ///< 1 1 0 1    True if unordered, less than, or equal
		//assert((isnan(lhsVal) || isnan(rhsVal) || lhsVal <= rhsVal) == actual && "FCMP_ULE");
		break;
	case llvm::FCmpInst::Predicate::FCMP_UNE:  ///< 1 1 1 0    True if unordered or not equal
		//assert((isnan(lhsVal) || isnan(rhsVal) || lhsVal != rhsVal) == actual && "FCMP_UNE");
		break;
	case llvm::FCmpInst::Predicate::FCMP_TRUE: ///< 1 1 1 1    Always true (always folded)
		assert(actual == true && "FCMP_TRUE");
		break;
	default:
		assert(false && "unsupported predicate");
		break;
	}
	cout << "comparison ok" << endl;
}


void FloatTest::instructionExecuted(const WorkItem *workItem,
                                        const llvm::Instruction *instruction,
                                        const TypedValue& result)
{
#ifdef DUMP_SHADOW
    cout << "++++++++++++++++++++++++++++++++++++++++++++" << endl;
    instruction->dump();
#endif

    instruction->dump();

    ShadowWorkItem *shadowWorkItem = shadowContext.getShadowWorkItem(workItem);
    ShadowValues *shadowValues = shadowWorkItem->getValues();

    switch(instruction->getOpcode())
    {

    	// FOR FLOAT TEST
    	case llvm::Instruction::Alloca:
		{
			const llvm::AllocaInst *allocaInst = ((const llvm::AllocaInst*)instruction);

			if(!allocaInst->getType()->isFloatTy()) break;

			size_t address = result.getPointer();

			shadowValues->setValue(instruction, ShadowContext::getUninitializedInterval());

			Interval* v = ShadowContext::getUninitializedInterval();
			allocAndStoreShadowMemory(AddrSpacePrivate, address, v, workItem);
			break;
		}

    	case llvm::Instruction::Store:
		{
			PARANOID_CHECK(workItem, instruction);
			const llvm::StoreInst *storeInst = ((const llvm::StoreInst*)instruction);
			const llvm::Value *Val = storeInst->getValueOperand();
			const llvm::Value *Addr = storeInst->getPointerOperand();

			size_t address = workItem->getOperand(Addr).getPointer();
			unsigned addrSpace = storeInst->getPointerAddressSpace();

			if(Val->getType()->isFloatTy()){

				if(const llvm::ConstantFP *fp = llvm::dyn_cast<const llvm::ConstantFP>(Val)){
					//Val is a constant

					const llvm::APFloat ap = fp->getValueAPF();
					float floatVal = fp->getValueAPF().convertToFloat();
					cout << floatVal << endl;

					Interval* shadowVal = ShadowContext::getIntervalFromFloat(floatVal);
					storeShadowMemory(addrSpace, address, shadowVal, workItem);
					shadowValues->setValue(Addr, shadowVal);

				}else{
					Interval* shadowVal = shadowContext.getValue(workItem, Val);
					cout << "not a float constant : " << shadowVal->lower() << endl;
					storeShadowMemory(addrSpace, address, shadowVal, workItem);
					shadowValues->setValue(Addr, shadowVal);
				}


			}

			break;
		}
		case llvm::Instruction::Load:
		{
			if(!instruction->getType()->isFloatTy()){
				cout << "not float type" << endl;
				break;
			}

			assert(instruction->getType()->isSized() && "Load type must have size");
			const llvm::LoadInst *loadInst = ((const llvm::LoadInst*)instruction);
			//const llvm::Value *Addr = loadInst->getPointerOperand();

			//size_t address = workItem->getOperand(Addr).getPointer();
			//unsigned addrSpace = loadInst->getPointerAddressSpace();

			Interval* v = ShadowContext::getIntervalFromFloat(result.getFloat(0));
			cout << "load : " << v->lower() << " " << v->upper() << endl;

			// I dont know why it was loading originally - Ask Moritz
			//loadShadowMemory(addrSpace, address, v, workItem);

			shadowValues->setValue(instruction, v);

			// Check shadow of address
			//TypedValue addrShadow = shadowContext.getValue(workItem, Addr);

			/*
			if(!ShadowContext::isCleanValue(addrShadow))
			{
				logUninitializedAddress(addrSpace, address, false);
			}
			*/

//            if (I.isAtomic())
//                I.setOrdering(addAcquireOrdering(I.getOrdering()));

			break;
		}
    	case llvm::Instruction::FAdd:
		{
			simpleFloatInstruction(workItem, instruction);
			break;
		}
		case llvm::Instruction::FDiv:
		{
			simpleFloatInstruction(workItem, instruction);
			break;
		}
		case llvm::Instruction::FMul:
		{
			simpleFloatInstruction(workItem, instruction);
			break;
		}
		case llvm::Instruction::FSub:
		{
			simpleFloatInstruction(workItem, instruction);
			break;
		}
		case llvm::Instruction::FCmp:
		{
			handleCmpInstruction(workItem, instruction, result);
			break;
		}

		// CASTS
        case llvm::Instruction::SIToFP:
        {
        	//take the result of cast and store value
        	const llvm::SIToFPInst* castInst = ((const llvm::SIToFPInst*) instruction);
        	if(!castInst->getDestTy()->isFloatTy()) break;
        	cout << "got cast to float" << endl;

        	Interval* v = ShadowContext::getIntervalFromFloat(result.getFloat(0));
        	shadowValues->setValue(instruction, v);

            break;
        }
        case llvm::Instruction::FPExt:
        {
        	//with intervals check if it contains the result
            break;
        }
        case llvm::Instruction::FPToSI:
        {
        	// convert to signed int and check if agrees with the result
        	const llvm::CastInst* castInst = ((const llvm::CastInst*) instruction);

        	if(!castInst->getSrcTy()->isFloatTy()) break;

        	llvm::Value *arg   = castInst->getOperand(0);
        	Interval* shadowValue = shadowValues->getValue(arg);

        	int64_t actual = result.getSInt(0);
        	int64_t shadow = 0;

        	switch(castInst->getDestTy()->getIntegerBitWidth()){
        	case 8:
        		//shadow = (int8_t)shadowValue.getFloat(0);
        		break;
        	case 16:
				//shadow = (int16_t)shadowValue.getFloat(0);
        		break;
        	case 32:
				//shadow = (int32_t)shadowValue.getFloat(0);
        		break;
        	case 64:
				//shadow = (int64_t)shadowValue.getFloat(0);
        		break;
        	default:
        		assert(false && "unsupported size");
        		break;
        	}
        	assert(actual == shadow && "wrong cast");
        	cout << "cast ok" << endl;
            break;
        }
        case llvm::Instruction::FPToUI:
        {
        	// convert to unsigned int and check if agrees with the result
			const llvm::CastInst* castInst = ((const llvm::CastInst*) instruction);

			if(!castInst->getSrcTy()->isFloatTy()) break;

			llvm::Value *arg   = castInst->getOperand(0);
			Interval* shadowValue = shadowValues->getValue(arg);

			uint64_t actual = result.getUInt(0);
			uint64_t shadow = 0;

			switch(castInst->getDestTy()->getIntegerBitWidth()){
			case 8:
				//shadow = (uint8_t)shadowValue.getFloat(0);
				break;
			case 16:
				//shadow = (uint16_t)shadowValue.getFloat(0);
				break;
			case 32:
				//shadow = (uint32_t)shadowValue.getFloat(0);
				break;
			case 64:
				//shadow = (uint64_t)shadowValue.getFloat(0);
				break;
			default:
				assert(false && "unsupported size");
				break;
			}
			assert(actual == shadow && "wrong cast");
			cout << "cast ok" << endl;
            break;
        }
        case llvm::Instruction::FPTrunc:
        {
        	const llvm::FPTruncInst *truncInst = ((const llvm::FPTruncInst*) instruction);
        	if(!truncInst->getDestTy()->isFloatTy()) break;

        	Interval* v = ShadowContext::getIntervalFromFloat(result.getFloat(0));
        	shadowValues->setValue(instruction, v);

            break;
        }
        case llvm::Instruction::FRem:
        {
        	// is this even allowed in openCL C?
            break;
        }
        case llvm::Instruction::UIToFP:
        {
        	//take the result of cast and store value
			const llvm::UIToFPInst* castInst = ((const llvm::UIToFPInst*) instruction);
			if(!castInst->getDestTy()->isFloatTy()) break;
			cout << "got cast to float" << endl;

			Interval* v = ShadowContext::getIntervalFromFloat(result.getFloat(0));
			shadowValues->setValue(instruction, v);
            break;
        }

		/////////////////////////

/*
        case llvm::Instruction::Add:
        {
            SimpleOr(workItem, instruction);
            break;
        }

        case llvm::Instruction::And:
        {
            SimpleOr(workItem, instruction);
            break;
        }
        case llvm::Instruction::AShr:
        {
            TypedValue S0 = shadowContext.getValue(workItem, instruction->getOperand(0));
            TypedValue S1 = shadowContext.getValue(workItem, instruction->getOperand(1));

            if(!ShadowContext::isCleanValue(S1))
            {
                shadowValues->setValue(instruction, ShadowContext::getPoisonedValue(instruction));
            }
            else
            {
                TypedValue newShadow = shadowContext.getMemoryPool()->clone(result);
                TypedValue Shift = workItem->getOperand(instruction->getOperand(1));
                uint64_t shiftMask = (S0.num > 1 ? S0.size : max((size_t)S0.size, sizeof(uint32_t))) * 8 - 1;

                for (unsigned i = 0; i < S0.num; i++)
                {
                    newShadow.setUInt(S0.getSInt(i) >> (Shift.getUInt(i) & shiftMask), i);
                }

                shadowValues->setValue(instruction, newShadow);
            }

            break;
        }
        case llvm::Instruction::BitCast:
        {
            TypedValue shadow = shadowContext.getValue(workItem, instruction->getOperand(0));
            TypedValue newShadow = shadowContext.getMemoryPool()->clone(result);

            memcpy(newShadow.data, shadow.data, newShadow.size*newShadow.num);
            shadowValues->setValue(instruction, newShadow);
            break;
        }
        case llvm::Instruction::Br:
        {
            checkAllOperandsDefined(workItem, instruction);
#ifdef DUMP_SHADOW
            // Insert pseudo value to keep numbering
            shadowValues->setValue(instruction, ShadowContext::getCleanValue(3));
#endif
            break;
        }
        case llvm::Instruction::Call:
        {
            const llvm::CallInst *callInst = ((const llvm::CallInst*)instruction);
            const llvm::Function *function = callInst->getCalledFunction();

            // Check for indirect function calls
            if (!function)
            {
                // Resolve indirect function pointer
                const llvm::Value *func = callInst->getCalledValue();
                const llvm::Value *funcPtr = ((const llvm::User*)func)->getOperand(0);
                function = (const llvm::Function*)funcPtr;
            }

            assert(!function->isVarArg() && "Variadic functions are not supported!");

            // For inline asm, do the usual thing: check argument shadow and mark all
            // outputs as clean. Note that any side effects of the inline asm that are
            // not immediately visible in its constraints are not handled.
            if (callInst->isInlineAsm())
            {
                checkAllOperandsDefined(workItem, instruction);
                shadowValues->setValue(instruction, ShadowContext::getCleanValue(instruction));
                break;
            }

            if(const llvm::IntrinsicInst *II = llvm::dyn_cast<const llvm::IntrinsicInst>(instruction))
            {
                handleIntrinsicInstruction(workItem, II);
                break;
            }

            if(function->isDeclaration())
            {
                if(!handleBuiltinFunction(workItem, function->getName().str(), callInst, result))
                {
                    // Handle external function calls
                    checkAllOperandsDefined(workItem, instruction);

                    if(callInst->getType()->isSized())
                    {
                        // Set return value only if function is non-void
                        shadowValues->setValue(instruction, ShadowContext::getCleanValue(instruction));
                    }
                }
                break;
            }

            assert(!llvm::isa<const llvm::IntrinsicInst>(instruction) && "intrinsics are handled elsewhere");

            // Fresh values for function
            ShadowFrame *values = shadowValues->createCleanShadowFrame();

            llvm::Function::const_arg_iterator argItr;
            for (argItr = function->arg_begin(); argItr != function->arg_end(); argItr++)
            {
                const llvm::Value *Val = callInst->getArgOperand(argItr->getArgNo());

                if (!Val->getType()->isSized())
                {
                    continue;
                }

                if(argItr->hasByValAttr())
                {
                    assert(Val->getType()->isPointerTy() && "ByVal argument is not a pointer!");
                    // Make new copy of shadow in private memory
                    size_t origShadowAddress = workItem->getOperand(Val).getPointer();
                    size_t newShadowAddress = workItem->getOperand(argItr).getPointer();
                    ShadowMemory *mem = shadowWorkItem->getPrivateMemory();
                    unsigned char *origShadowData = (unsigned char*)mem->getPointer(origShadowAddress);
                    size_t size = getTypeSize(argItr->getType()->getPointerElementType());

                    // Set new shadow memory
                    TypedValue v = ShadowContext::getCleanValue(size);
                    memcpy(v.data, origShadowData, size);
                    allocAndStoreShadowMemory(AddrSpacePrivate, newShadowAddress, v, workItem);
                    values->setValue(argItr, ShadowContext::getCleanValue(argItr));
                }
                else
                {
                    TypedValue newShadow = shadowContext.getMemoryPool()->clone(shadowContext.getValue(workItem, Val));
                    values->setValue(argItr, newShadow);
                }
            }

            // Now, get the shadow for the RetVal.
            if(callInst->getType()->isSized())
            {
                values->setCall(callInst);
            }

            shadowValues->pushFrame(values);

            break;
        }
        case llvm::Instruction::ExtractElement:
        {
            const llvm::ExtractElementInst *extractInst = ((const llvm::ExtractElementInst*)instruction);

            TypedValue indexShadow = shadowContext.getValue(workItem, extractInst->getIndexOperand());

            if(!ShadowContext::isCleanValue(indexShadow))
            {
                logUninitializedIndex();
            }

            TypedValue vectorShadow = shadowContext.getValue(workItem, extractInst->getVectorOperand());
            TypedValue newShadow = shadowContext.getMemoryPool()->clone(result);

            unsigned index = workItem->getOperand(extractInst->getIndexOperand()).getUInt();
            memcpy(newShadow.data, vectorShadow.data + newShadow.size*index, newShadow.size);

            shadowValues->setValue(instruction, newShadow);
            break;
        }
        case llvm::Instruction::ExtractValue:
        {
            const llvm::ExtractValueInst *extractInst = ((const llvm::ExtractValueInst*)instruction);

            const llvm::Value *Agg = extractInst->getAggregateOperand();
            TypedValue ResShadow = shadowContext.getMemoryPool()->clone(result);

            llvm::ArrayRef<unsigned int> indices = extractInst->getIndices();

            // Compute offset for target value
            int offset = 0;
            const llvm::Type *type = Agg->getType();
            for (unsigned i = 0; i < indices.size(); i++)
            {
                if (type->isArrayTy())
                {
                    type = type->getArrayElementType();
                    offset += getTypeSize(type) * indices[i];
                }
                else if (type->isStructTy())
                {
                    offset += getStructMemberOffset((const llvm::StructType*)type, indices[i]);
                    type = type->getStructElementType(indices[i]);
                }
                else
                {
                    FATAL_ERROR("Unsupported aggregate type: %d", type->getTypeID())
                }
            }

            // Copy target value to result
            memcpy(ResShadow.data, shadowContext.getValue(workItem, Agg).data + offset, getTypeSize(type));

            shadowValues->setValue(instruction, ResShadow);
            break;
        }



        case llvm::Instruction::GetElementPtr:
        {
            SimpleOr(workItem, instruction);
            break;
        }
        case llvm::Instruction::ICmp:
        {
            SimpleOr(workItem, instruction);
            break;
        }
        case llvm::Instruction::InsertElement:
        {
            TypedValue indexShadow = shadowContext.getValue(workItem, instruction->getOperand(2));

            if(!ShadowContext::isCleanValue(indexShadow))
            {
                logUninitializedIndex();
            }

            TypedValue vectorShadow = shadowContext.getValue(workItem, instruction->getOperand(0));
            TypedValue elementShadow = shadowContext.getValue(workItem, instruction->getOperand(1));
            TypedValue newShadow = shadowContext.getMemoryPool()->clone(result);

            unsigned index = workItem->getOperand(instruction->getOperand(2)).getUInt();
            memcpy(newShadow.data, vectorShadow.data, newShadow.size*newShadow.num);
            memcpy(newShadow.data + index*newShadow.size, elementShadow.data, newShadow.size);

            shadowValues->setValue(instruction, newShadow);
            break;
        }
        case llvm::Instruction::InsertValue:
        {
            const llvm::InsertValueInst *insertInst = (const llvm::InsertValueInst*)instruction;

            TypedValue newShadow = shadowContext.getMemoryPool()->clone(result);

            // Load original aggregate data
            const llvm::Value *agg = insertInst->getAggregateOperand();
            memcpy(newShadow.data, shadowContext.getValue(workItem, agg).data, newShadow.size*newShadow.num);

            // Compute offset for inserted value
            int offset = 0;
            llvm::ArrayRef<unsigned int> indices = insertInst->getIndices();
            const llvm::Type *type = agg->getType();
            for (unsigned i = 0; i < indices.size(); i++)
            {
                if (type->isArrayTy())
                {
                    type = type->getArrayElementType();
                    offset += getTypeSize(type) * indices[i];
                }
                else if (type->isStructTy())
                {
                    offset += getStructMemberOffset((const llvm::StructType*)type, indices[i]);
                    type = type->getStructElementType(indices[i]);
                }
                else
                {
                    FATAL_ERROR("Unsupported aggregate type: %d", type->getTypeID())
                }
            }

            // Copy inserted value into result
            const llvm::Value *value = insertInst->getInsertedValueOperand();
            memcpy(newShadow.data + offset, shadowContext.getValue(workItem, value).data, getTypeSize(value->getType()));

            shadowValues->setValue(instruction, newShadow);
            break;
        }
        case llvm::Instruction::IntToPtr:
        {
            TypedValue shadow = shadowContext.getValue(workItem, instruction->getOperand(0));
            TypedValue newShadow = shadowContext.getMemoryPool()->clone(result);

            for (unsigned i = 0; i < newShadow.num; i++)
            {
                newShadow.setPointer(shadow.getUInt(i), i);
            }

            shadowValues->setValue(instruction, newShadow);
            break;
        }

        case llvm::Instruction::LShr:
        {
            TypedValue S0 = shadowContext.getValue(workItem, instruction->getOperand(0));
            TypedValue S1 = shadowContext.getValue(workItem, instruction->getOperand(1));

            if(!ShadowContext::isCleanValue(S1))
            {
                shadowValues->setValue(instruction, ShadowContext::getPoisonedValue(instruction));
            }
            else
            {
                TypedValue newShadow = shadowContext.getMemoryPool()->clone(result);
                TypedValue Shift = workItem->getOperand(instruction->getOperand(1));
                uint64_t shiftMask = (S0.num > 1 ? S0.size : max((size_t)S0.size, sizeof(uint32_t))) * 8 - 1;

                for (unsigned i = 0; i < S0.num; i++)
                {
                    newShadow.setUInt(S0.getUInt(i) >> (Shift.getUInt(i) & shiftMask), i);
                }

                shadowValues->setValue(instruction, newShadow);
            }

            break;
        }
        case llvm::Instruction::Mul:
        {
            SimpleOr(workItem, instruction);
            break;
        }
        case llvm::Instruction::Or:
        {
            SimpleOr(workItem, instruction);
            break;
        }
        case llvm::Instruction::PHI:
        {
            const llvm::PHINode *phiNode = (const llvm::PHINode*)instruction;
            const llvm::Value *value = phiNode->getIncomingValueForBlock(workItem->getPreviousBlock());
            TypedValue shadowValue = shadowContext.getValue(workItem, value);

            shadowValues->setValue(instruction, shadowValue);
            break;
        }
        case llvm::Instruction::PtrToInt:
        {
            TypedValue shadow = shadowContext.getValue(workItem, instruction->getOperand(0));
            TypedValue newShadow = shadowContext.getMemoryPool()->clone(result);

            for (unsigned i = 0; i < newShadow.num; i++)
            {
                newShadow.setUInt(shadow.getPointer(i), i);
            }

            shadowValues->setValue(instruction, newShadow);
            break;
        }
        case llvm::Instruction::Ret:
        {
            const llvm::ReturnInst *retInst = ((const llvm::ReturnInst*)instruction);
            const llvm::Value *RetVal = retInst->getReturnValue();

            if(RetVal)
            {
                //Value *ShadowPtr = getValuePtrForRetval(RetVal, IRB);
                //if (CheckReturnValue) {
                //    insertShadowCheck(RetVal, &I);
                //    Value *Shadow = getCleanValue(RetVal);
                //    IRB.CreateAlignedStore(Shadow, ShadowPtr, kShadowTLSAlignment);
                //} else {
                TypedValue retValShadow = shadowContext.getMemoryPool()->clone(shadowContext.getValue(workItem, RetVal));
                const llvm::CallInst *callInst = shadowValues->getCall();
                shadowValues->popFrame();
                shadowValues->setValue(callInst, retValShadow);
                //}
            }
            else
            {
#ifdef DUMP_SHADOW
                // Insert pseudo value to keep numbering
                shadowValues->setValue(instruction, ShadowContext::getCleanValue(3));
#endif
                shadowValues->popFrame();
            }

            break;
        }
        case llvm::Instruction::SDiv:
        {
            SimpleOr(workItem, instruction);
            break;
        }
        case llvm::Instruction::Select:
        {
            const llvm::SelectInst *selectInst = (const llvm::SelectInst*)instruction;

            TypedValue opCondition = workItem->getOperand(selectInst->getCondition());
            TypedValue conditionShadow = shadowContext.getValue(workItem, selectInst->getCondition());
            TypedValue newShadow;

            if(!ShadowContext::isCleanValue(conditionShadow))
            {
                newShadow = ShadowContext::getPoisonedValue(instruction);
            }
            else
            {
                newShadow = shadowContext.getMemoryPool()->clone(result);

                for(unsigned i = 0; i < result.num; i++)
                {
                    const bool cond = selectInst->getCondition()->getType()->isVectorTy() ?
                        opCondition.getUInt(i) :
                        opCondition.getUInt();
                    const llvm::Value *op = cond ?
                        selectInst->getTrueValue() :
                        selectInst->getFalseValue();

                    memcpy(newShadow.data + i*newShadow.size,
                            shadowContext.getValue(workItem, op).data + i*newShadow.size,
                            newShadow.size);
                }
            }

            shadowValues->setValue(instruction, newShadow);
            break;
        }
        case llvm::Instruction::SExt:
        {
            const llvm::Value *operand = instruction->getOperand(0);
            TypedValue shadow = shadowContext.getValue(workItem, operand);
            TypedValue newShadow = shadowContext.getMemoryPool()->clone(result);

            for (unsigned i = 0; i < newShadow.num; i++)
            {
                int64_t val = shadow.getSInt(i);
                if (operand->getType()->getPrimitiveSizeInBits() == 1)
                {
                    val = val ? -1 : 0;
                }
                newShadow.setSInt(val, i);
            }

            shadowValues->setValue(instruction, newShadow);

            break;
        }
        case llvm::Instruction::Shl:
        {
            TypedValue S0 = shadowContext.getValue(workItem, instruction->getOperand(0));
            TypedValue S1 = shadowContext.getValue(workItem, instruction->getOperand(1));

            if(!ShadowContext::isCleanValue(S1))
            {
                shadowValues->setValue(instruction, ShadowContext::getPoisonedValue(instruction));
            }
            else
            {
                TypedValue newShadow = shadowContext.getMemoryPool()->clone(result);
                TypedValue Shift = workItem->getOperand(instruction->getOperand(1));
                uint64_t shiftMask = (S0.num > 1 ? S0.size : max((size_t)S0.size, sizeof(uint32_t))) * 8 - 1;

                for (unsigned i = 0; i < S0.num; i++)
                {
                    newShadow.setUInt(S0.getUInt(i) << (Shift.getUInt(i) & shiftMask), i);
                }

                shadowValues->setValue(instruction, newShadow);
            }

            break;
        }
        case llvm::Instruction::ShuffleVector:
        {
            const llvm::ShuffleVectorInst *shuffleInst = (const llvm::ShuffleVectorInst*)instruction;
            const llvm::Value *v1 = shuffleInst->getOperand(0);
            const llvm::Value *v2 = shuffleInst->getOperand(1);
            TypedValue mask = workItem->getOperand(shuffleInst->getMask());
            TypedValue maskShadow = shadowContext.getValue(workItem, shuffleInst->getMask());
            TypedValue newShadow = shadowContext.getMemoryPool()->clone(result);
            TypedValue pv = ShadowContext::getPoisonedValue(newShadow.size);

            for(unsigned i = 0; i < newShadow.num; i++)
            {
                if(shuffleInst->getMask()->getAggregateElement(i)->getValueID() == llvm::Value::UndefValueVal)
                {
                    // Don't care / undef
                    continue;
                }

                const llvm::Value *src = v1;
                unsigned int index = mask.getUInt(i);
                if(index >= newShadow.num)
                {
                    index -= newShadow.num;
                    src = v2;
                }

                if(!ShadowContext::isCleanValue(maskShadow, i))
                {
                    memcpy(newShadow.data + i*newShadow.size, pv.data, newShadow.size);
                }
                else
                {
                    TypedValue v = shadowContext.getValue(workItem, src);
                    size_t srcOffset = index*newShadow.size;
                    memcpy(newShadow.data + i*newShadow.size, v.data + srcOffset, newShadow.size);
                }
            }

            shadowValues->setValue(instruction, newShadow);
            break;
        }

        case llvm::Instruction::SRem:
        {
            SimpleOr(workItem, instruction);
            break;
        }

        case llvm::Instruction::Sub:
        {
            SimpleOr(workItem, instruction);
            break;
        }
        case llvm::Instruction::Switch:
        {
            checkAllOperandsDefined(workItem, instruction);
#ifdef DUMP_SHADOW
            // Insert pseudo value to keep numbering
            shadowValues->setValue(instruction, ShadowContext::getCleanValue(3));
#endif
            break;
        }
        case llvm::Instruction::Trunc:
        {
            TypedValue shadow = shadowContext.getValue(workItem, instruction->getOperand(0));
            TypedValue newShadow = shadowContext.getMemoryPool()->clone(result);

            for (unsigned i = 0; i < newShadow.num; i++)
            {
                memcpy(newShadow.data+i*newShadow.size, shadow.data+i*shadow.size, newShadow.size);
            }

            shadowValues->setValue(instruction, newShadow);
            break;
        }
        case llvm::Instruction::UDiv:
        {
            SimpleOr(workItem, instruction);
            break;
        }

        case llvm::Instruction::URem:
        {
            SimpleOr(workItem, instruction);
            break;
        }
        case llvm::Instruction::Unreachable:
            FATAL_ERROR("Encountered unreachable instruction");
        case llvm::Instruction::Xor:
        {
            SimpleOr(workItem, instruction);
            break;
        }
        case llvm::Instruction::ZExt:
        {
            TypedValue shadow = shadowContext.getValue(workItem, instruction->getOperand(0));
            TypedValue newShadow = shadowContext.getMemoryPool()->clone(result);

            for (unsigned i = 0; i < newShadow.num; i++)
            {
                newShadow.setUInt(shadow.getUInt(i), i);
            }

            shadowValues->setValue(instruction, newShadow);
            break;
        }
        */
        default:
        	if(instruction->getType()->isFloatTy()){
        		cout << "unsuppoted float operation" << endl;
        	}
        	//FATAL_ERROR("Unsupported instruction: %s", instruction->getOpcodeName());
        	break;
    }
}

void FloatTest::kernelBegin(const KernelInvocation *kernelInvocation)
{
    const Kernel *kernel = kernelInvocation->getKernel();

    // Initialise kernel arguments and global variables
    for (auto value = kernel->values_begin(); value != kernel->values_end(); value++)
    {
        const llvm::Type *type = value->first->getType();

        if(!type->isSized())
        {
            continue;
        }

        if(type->isPointerTy())
        {
            switch(type->getPointerAddressSpace())
            {
                case AddrSpaceConstant:
                {
                    shadowContext.setGlobalValue(value->first, ShadowContext::getUninitializedInterval());
                    const llvm::Type *elementTy = type->getPointerElementType();
                    allocAndStoreShadowMemory(AddrSpaceConstant, value->second.getPointer(),
                                              ShadowContext::getUninitializedInterval());
                    break;
                }
                case AddrSpaceGlobal:
                {
                    // Global pointer kernel arguments
                    // value->second.data == ptr
                    // value->second.size == ptr size
                    size_t address = value->second.getPointer();

                    if(m_context->getGlobalMemory()->isAddressValid(address) &&
                       !shadowContext.getGlobalMemory()->isAddressValid(address))
                    {
                        // Allocate poisoned global memory if there was no host store
                        size_t size = m_context->getGlobalMemory()->getBuffer(address)->size;
                        allocAndStoreShadowMemory(AddrSpaceGlobal, address,
                                                  ShadowContext::getUninitializedInterval(), NULL, NULL, true);
                    }

                    m_deferredInit.push_back(*value);
                    break;
                }
                case AddrSpaceLocal:
                {
                    // Local pointer kernel arguments and local data variables
                    // value->second.data == NULL
                    // value->second.size == val size
                    if(llvm::isa<llvm::Argument>(value->first))
                    {
                        // Arguments have a private pointer
                        m_deferredInit.push_back(*value);
                    }
                    else
                    {
                        // Variables have a global pointer
                        shadowContext.setGlobalValue(value->first, ShadowContext::getUninitializedInterval());
                    }

                    m_deferredInitGroup.push_back(*value);
                    break;
                }
                case AddrSpacePrivate:
                {
                    const llvm::Argument *A = llvm::dyn_cast<llvm::Argument>(value->first);

                    if(A && A->hasByValAttr())
                    {
                        // ByVal kernel argument
                        // value->second.data == val
                        // value->second.size == val size
                        m_deferredInit.push_back(*value);
                    }
                    else
                    {
                        // Private struct/Union definitions with global type
                        // value->second.data == val
                        // value->second.size == val size
                        m_deferredInit.push_back(*value);
                        shadowContext.setGlobalValue(value->first, ShadowContext::getUninitializedInterval());
                    }
                    break;
                }
                default:
                    FATAL_ERROR("Unsupported addressspace %d", type->getPointerAddressSpace());
            }
        }
        else
        {
            // Non pointer type kernel arguments
            // value->second.data == val
            // value->second.size == val size
            m_deferredInit.push_back(*value);
        }
    }
}

Interval* FloatTest::loadShadowMemory(unsigned addrSpace, size_t address, const WorkItem *workItem, const WorkGroup *workGroup)
{
    if(addrSpace == AddrSpaceConstant)
    {
        //TODO: Eventually load value
        //memset(SM.data, 0, SM.size*SM.num);
        return NULL;
    }

    ShadowMemory *memory = getShadowMemory(addrSpace, workItem, workGroup);
    return memory->load(address);
}

void FloatTest::logUninitializedAddress(unsigned int addrSpace, size_t address, bool write) const
{
  Context::Message msg(WARNING, m_context);
  msg << "Uninitialized address used to " << (write ? "write to " : "read from ")
      << getAddressSpaceName(addrSpace)
      << " memory address 0x" << hex << address << endl
      << msg.INDENT
      << "Kernel: " << msg.CURRENT_KERNEL << endl
      << "Entity: " << msg.CURRENT_ENTITY << endl
      << msg.CURRENT_LOCATION << endl;
  msg.send();
}

void FloatTest::logUninitializedCF() const
{
  Context::Message msg(WARNING, m_context);
  msg << "Controlflow depends on uninitialized value" << endl
      << msg.INDENT
      << "Kernel: " << msg.CURRENT_KERNEL << endl
      << "Entity: " << msg.CURRENT_ENTITY << endl
      << msg.CURRENT_LOCATION << endl;
  msg.send();
}

void FloatTest::logUninitializedIndex() const
{
  Context::Message msg(WARNING, m_context);
  msg << "Instruction depends on an uninitialized index value" << endl
      << msg.INDENT
      << "Kernel: " << msg.CURRENT_KERNEL << endl
      << "Entity: " << msg.CURRENT_ENTITY << endl
      << msg.CURRENT_LOCATION << endl;
  msg.send();
}

void FloatTest::logUninitializedWrite(unsigned int addrSpace, size_t address) const
{
  Context::Message msg(WARNING, m_context);
  msg << "Uninitialized value written to "
      << getAddressSpaceName(addrSpace)
      << " memory address 0x" << hex << address << endl
      << msg.INDENT
      << "Kernel: " << msg.CURRENT_KERNEL << endl
      << "Entity: " << msg.CURRENT_ENTITY << endl
      << msg.CURRENT_LOCATION << endl;
  msg.send();
}

void FloatTest::memoryMap(const Memory *memory, size_t address,
                                      size_t offset, size_t size, cl_map_flags flags)
{
    if(!(flags & CL_MAP_READ))
    {
        allocAndStoreShadowMemory(memory->getAddressSpace(), address + offset,
                ShadowContext::getUninitializedInterval());
    }
}

/*
void FloatTest::SimpleOr(const WorkItem *workItem, const llvm::Instruction *I)
{
    PARANOID_CHECK(workItem, I);
    ShadowValues *shadowValues = shadowContext.getShadowWorkItem(workItem)->getValues();

    for(llvm::Instruction::const_op_iterator OI = I->op_begin(); OI != I->op_end(); ++OI)
    {
        if(!ShadowContext::isCleanValue(shadowContext.getValue(workItem, OI->get())))
        {
            shadowValues->setValue(I, ShadowContext::getPoisonedValue(I));
            return;
        }
    }

    shadowValues->setValue(I, ShadowContext::getCleanValue(I));
}
*/

// this isn't used anywhere
/*
void FloatTest::SimpleOrAtomic(const WorkItem *workItem, const llvm::CallInst *CI)
{
    const llvm::Value *Addr = CI->getArgOperand(0);
    unsigned addrSpace = Addr->getType()->getPointerAddressSpace();
    size_t address = workItem->getOperand(Addr).getPointer();
    Interval argShadow = shadowContext.getValue(workItem, CI->getArgOperand(1));
    TypedValue oldShadow = {
        4,
        1,
        shadowContext.getMemoryPool()->alloc(4)
    };

    TypedValue newShadow;

    if(addrSpace == AddrSpaceGlobal)
    {
        shadowContext.getGlobalMemory()->lock(address);
    }

    loadShadowMemory(addrSpace, address, oldShadow, workItem);


    if(!ShadowContext::isCleanValue(argShadow) || !ShadowContext::isCleanValue(oldShadow))
    {
        newShadow = ShadowContext::getPoisonedValue(4);
    }
    else
    {
        newShadow = ShadowContext::getCleanValue(4);
    }


    storeShadowMemory(addrSpace, address, newShadow, workItem);

    if(addrSpace == AddrSpaceGlobal)
    {
        shadowContext.getGlobalMemory()->unlock(address);
    }

    ShadowValues *shadowValues = shadowContext.getShadowWorkItem(workItem)->getValues();
    shadowValues->setValue(CI, oldShadow);

    // Check shadow of address
    TypedValue addrShadow = shadowContext.getValue(workItem, Addr);


    if(!ShadowContext::isCleanValue(addrShadow))
    {
        logUninitializedAddress(addrSpace, address);
    }

}
*/

void FloatTest::storeShadowMemory(unsigned addrSpace, size_t address, Interval* inter, const WorkItem *workItem, const WorkGroup *workGroup, bool unchecked)
{
#ifdef DUMP_SHADOW
    cout << "Store " << hex << SM << " to space " << dec << addrSpace << " at address " << hex << address << endl;
#endif

    /*
    if(!unchecked && addrSpace != AddrSpacePrivate && !ShadowContext::isCleanValue(SM))
    {
#ifdef DUMP_SHADOW
        shadowContext.dump(workItem);
#endif
        logUninitializedWrite(addrSpace, address);
    }
    */

    if(addrSpace == AddrSpaceConstant)
    {
    	cout << "got constant" << endl;
        //TODO: Eventually store value
        return;
    }

    ShadowMemory *memory = getShadowMemory(addrSpace, workItem, workGroup);
    memory->store(inter, address);
}

// TODO : store floats
void FloatTest::workItemBegin(const WorkItem *workItem)
{
    shadowContext.createMemoryPool();
    shadowContext.allocateWorkItems();
    ShadowWorkItem *shadowWI = shadowContext.createShadowWorkItem(workItem);
    ShadowValues *shadowValues = shadowWI->getValues();

    for(auto value : m_deferredInit)
    {
        const llvm::Type *type = value.first->getType();

        if(type->isPointerTy())
        {
            switch(type->getPointerAddressSpace())
            {
                case AddrSpaceGlobal:
                {
                    // Global pointer kernel arguments
                    // value.second.data == ptr
                    // value.second.size == ptr size
                    shadowValues->setValue(value.first, ShadowContext::getUninitializedInterval());
                    break;
                }
                case AddrSpaceLocal:
                {
                    // Local pointer kernel arguments
                    // value.second.data == NULL
                    // value.second.size == val size
                    shadowValues->setValue(value.first, ShadowContext::getUninitializedInterval());
                    break;
                }
                case AddrSpacePrivate:
                {
                    const llvm::Argument *A = llvm::dyn_cast<llvm::Argument>(value.first);

                    if(A && A->hasByValAttr())
                    {
                        // ByVal kernel argument
                        // value.second.data == val
                        // value.second.size == val size
                        size_t address = workItem->getOperand(value.first).getPointer();
                        allocAndStoreShadowMemory(AddrSpacePrivate, address,
                        			ShadowContext::getUninitializedInterval(), workItem);
                        shadowValues->setValue(value.first, ShadowContext::getUninitializedInterval());
                    }
                    else
                    {
                        // Private struct/Union definitions with global type
                        // value.second.data == NULL
                        // value.second.size == val size
                        size_t address = workItem->getOperand(value.first).getPointer();
                        allocAndStoreShadowMemory(AddrSpacePrivate, address,
                        		ShadowContext::getUninitializedInterval(), workItem);
                    }
                    break;
                }
            }
        }
        else
        {
            // Non pointer type kernel arguments
            // value->second.data == val
            // value->second.size == val size
            shadowValues->setValue(value.first, ShadowContext::getUninitializedInterval());
        }
    }
}

void FloatTest::workItemComplete(const WorkItem *workItem)
{
    shadowContext.destroyShadowWorkItem(workItem);
    shadowContext.freeWorkItems();
    shadowContext.destroyMemoryPool();
}

void FloatTest::workGroupBegin(const WorkGroup *workGroup)
{
    shadowContext.createMemoryPool();
    shadowContext.allocateWorkGroups();
    shadowContext.createShadowWorkGroup(workGroup);

    for(auto value : m_deferredInitGroup)
    {
        // Local data variables
        // value->second.data == NULL
        // value->second.size == val size
        size_t address = workGroup->getLocalMemoryAddress(value.first);
        Interval* v;

        if(llvm::isa<llvm::Argument>(value.first))
        {
            //TODO: Local memory clean or poisoned? May need to differentiate
            //      between kernel argument (?) and variable (poisoned)
            //v = ShadowContext::getPoisonedValue(value.second.size);
            v = ShadowContext::getUninitializedInterval();

        }
        else
        {
            //v = ShadowContext::getPoisonedValue(value.second.size);
            v = ShadowContext::getUninitializedInterval();

        }

        allocAndStoreShadowMemory(AddrSpaceLocal, address, v, NULL, workGroup, true);
    }
}

void FloatTest::workGroupComplete(const WorkGroup *workGroup)
{
    shadowContext.destroyShadowWorkGroup(workGroup);
    shadowContext.freeWorkGroups();
}

ShadowFrame::ShadowFrame() :
    m_values(new UnorderedIntervalMap())
{
#ifdef DUMP_SHADOW
    m_valuesList = new ValuesList();
#endif
}

ShadowFrame::~ShadowFrame()
{
    delete m_values;
#ifdef DUMP_SHADOW
    delete m_valuesList;
#endif
}

void ShadowFrame::dump() const
{
    cout << "==== ShadowMap (private) =======" << endl;

#ifdef DUMP_SHADOW
    ValuesList::const_iterator itr;
    unsigned num = 1;

    for(itr = m_valuesList->begin(); itr != m_valuesList->end(); ++itr)
    {
        if((*itr)->hasName())
        {
            cout << "%" << (*itr)->getName().str() << ": " << m_values->at(*itr) << endl;
        }
        else
        {
            cout << "%" << dec << num++ << ": " << m_values->at(*itr) << endl;
        }
    }
#else
    cout << endl << "Dump not activated!" << endl;
#endif

    cout << "=======================" << endl;
}

Interval* ShadowFrame::getValue(const llvm::Value *V) const
{
    if (llvm::isa<llvm::Instruction>(V)) {
        // For instructions the shadow is already stored in the map.
        assert(m_values->count(V) && "No shadow for instruction value");
        return m_values->at(V);
    }
    else if (llvm::isa<llvm::UndefValue>(V)) {
    	// TODO : don't know if this case is handled correctly
        return ShadowContext::getUninitializedInterval();
    }
    else if (llvm::isa<llvm::Argument>(V)) {
        // For arguments the shadow is already stored in the map.
        assert(m_values->count(V) && "No shadow for argument value");
        return m_values->at(V);
    }
    else if(const llvm::ConstantVector *VC = llvm::dyn_cast<llvm::ConstantVector>(V))
    {
        //TODO : handle vectors
    }
    else
    {
        // For everything else the shadow is zero.
        return ShadowContext::getUninitializedInterval();
    }
}

void ShadowFrame::setValue(const llvm::Value *V, Interval* inter)
{
#ifdef DUMP_SHADOW
    if(!m_values->count(V))
    {
        m_valuesList->push_back(V);
    }
    else
    {
        cout << "Shadow for value " << V->getName().str() << " reset!" << endl;
    }
#endif
    (*m_values)[V] = inter;
}

ShadowValues::ShadowValues() :
    m_stack(new ShadowValuesStack())
{
    pushFrame(createCleanShadowFrame());
}

ShadowValues::~ShadowValues()
{
    while(!m_stack->empty())
    {
        ShadowFrame *frame = m_stack->top();
        m_stack->pop();
        delete frame;
    }

    delete m_stack;
}

ShadowFrame* ShadowValues::createCleanShadowFrame()
{
    return new ShadowFrame();
}

ShadowWorkItem::ShadowWorkItem(unsigned bufferBits) :
    m_memory(new ShadowMemory(AddrSpacePrivate, bufferBits)), m_values(new ShadowValues())
{
}

ShadowWorkItem::~ShadowWorkItem()
{
    delete m_memory;
    delete m_values;
}

ShadowWorkGroup::ShadowWorkGroup(unsigned bufferBits) :
    //FIXME: Hard coded values
    m_memory(new ShadowMemory(AddrSpaceLocal, sizeof(size_t) == 8 ? 16 : 8))
{
}

ShadowWorkGroup::~ShadowWorkGroup()
{
    delete m_memory;
}

ShadowMemory::ShadowMemory(AddressSpace addrSpace, unsigned bufferBits) :
    m_addrSpace(addrSpace), m_map(), m_numBitsAddress((sizeof(size_t)<<3) - bufferBits), m_numBitsBuffer(bufferBits)
{
}

ShadowMemory::~ShadowMemory()
{
    clear();
}

void ShadowMemory::allocate(size_t address)
{
    size_t index = extractBuffer(address);

    if(m_map.count(index))
    {
        deallocate(address);
    }

    /*
    Buffer *buffer = new Buffer();
    buffer->size   = size;
    buffer->flags  = 0;
    buffer->data   = new unsigned char[size];
	*/

    Interval *inter = new Interval(-INF, INF);

    m_map[index] = inter;
}

void ShadowMemory::clear()
{
    MemoryMap::iterator mItr;
    for(mItr = m_map.begin(); mItr != m_map.end(); ++mItr)
    {
        //delete[] mItr->second->data;
        delete mItr->second;
    }
}

void ShadowMemory::deallocate(size_t address)
{
    //size_t index = extractBuffer(address);

    assert(m_map.count(address) && "Cannot deallocate non existing memory!");

    delete m_map.at(address);
    m_map.at(address) = NULL;
}

void ShadowMemory::dump() const
{
    cout << "====== ShadowMem (" << getAddressSpaceName(m_addrSpace) << ") ======";

    // TODO : rewrite this for intervals
    /*
    for(unsigned b = 0, o = 1; b < m_map.size(); o++)
    {
        if(!m_map.count(b+o))
        {
            continue;
        }

        for(unsigned i = 0; i < m_map.at(b+o)->size; i++)
        {
            if (i%4 == 0)
            {
                cout << endl << hex << uppercase
                    << setw(16) << setfill(' ') << right
                    << ((((size_t)b+o)<<m_numBitsAddress) | i) << ":";
            }
            cout << " " << hex << uppercase << setw(2) << setfill('0')
                << (int)m_map.at(b+o)->data[i];
        }

        ++b;
        o = 0;
    }
    */
    cout << endl;

    cout << "=======================" << endl;
}

size_t ShadowMemory::extractBuffer(size_t address) const
{
    return (address >> m_numBitsAddress);
}

size_t ShadowMemory::extractOffset(size_t address) const
{
    return (address & (((size_t)-1) >> m_numBitsBuffer));
}

void* ShadowMemory::getPointer(size_t address) const
{
    //size_t index = extractBuffer(address);
    //size_t offset= extractOffset(address);

    assert(m_map.count(address) && "No shadow memory found!");

    return m_map.at(address);
}

bool ShadowMemory::isAddressValid(size_t address) const
{
    //size_t index = extractBuffer(address);
    //size_t offset = extractOffset(address);
    return m_map.count(address);
}

Interval* ShadowMemory::load(size_t address) const
{
    //size_t index = extractBuffer(address);
    //size_t offset = extractOffset(address);

    if(isAddressValid(address))
    {
        assert(m_map.count(address) && "No shadow memory found!");
        //memcpy(dst, m_map.at(index)->data + offset, size);
        return m_map.at(address);
    }
    else
    {
    	cout << "poisoned load" << endl;
    	assert(false && "couldn't find interval at this address");
        //TypedValue v = ShadowContext::getPoisonedValue(size);
       // memcpy(dst, v.data, size);
    }
}

void ShadowMemory::lock(size_t address) const
{
    size_t offset = extractOffset(address);
    ATOMIC_MUTEX(offset).lock();
}

void ShadowMemory::store(/*const unsigned char *src*/ Interval* inter, size_t address)
{
    //size_t index = extractBuffer(address);
    //size_t offset = extractOffset(address);

    if(isAddressValid(address))
    {
        assert(m_map.count(address) && "Cannot store to unallocated memory!");
        //memcpy(m_map.at(index)->data + offset, src, size);
        m_map.at(address) = inter;
    }
}

void ShadowMemory::unlock(size_t address) const
{
    size_t offset = extractOffset(address);
    ATOMIC_MUTEX(offset).unlock();
}

ShadowContext::ShadowContext(unsigned bufferBits) :
    m_globalMemory(new ShadowMemory(AddrSpaceGlobal, bufferBits)), m_globalValues(), m_numBitsBuffer(bufferBits)
{
}

ShadowContext::~ShadowContext()
{
    delete m_globalMemory;
}

void ShadowContext::allocateWorkItems()
{
    if(!m_workSpace.workItems)
    {
        m_workSpace.workItems = new ShadowItemMap();
    }
}

void ShadowContext::allocateWorkGroups()
{
    if(!m_workSpace.workGroups)
    {
        m_workSpace.workGroups = new ShadowGroupMap();
    }
}

void ShadowContext::createMemoryPool()
{
    if(m_workSpace.poolUsers == 0)
    {
        m_workSpace.memoryPool = new MemoryPool();
    }

    ++m_workSpace.poolUsers;
}

ShadowWorkItem* ShadowContext::createShadowWorkItem(const WorkItem *workItem)
{
    assert(!m_workSpace.workItems->count(workItem) && "Workitems may only have one shadow");
    ShadowWorkItem *sWI = new ShadowWorkItem(m_numBitsBuffer);
    (*m_workSpace.workItems)[workItem] = sWI;
    return sWI;
}

ShadowWorkGroup* ShadowContext::createShadowWorkGroup(const WorkGroup *workGroup)
{
    assert(!m_workSpace.workGroups->count(workGroup) && "Workgroups may only have one shadow");
    ShadowWorkGroup *sWG = new ShadowWorkGroup(m_numBitsBuffer);
    (*m_workSpace.workGroups)[workGroup] = sWG;
    return sWG;
}

void ShadowContext::destroyMemoryPool()
{
    --m_workSpace.poolUsers;

    if(m_workSpace.poolUsers == 0)
    {
        delete m_workSpace.memoryPool;
    }
}

void ShadowContext::destroyShadowWorkItem(const WorkItem *workItem)
{
    assert(m_workSpace.workItems->count(workItem) && "No shadow for workitem found!");
    delete (*m_workSpace.workItems)[workItem];
    m_workSpace.workItems->erase(workItem);
}

void ShadowContext::destroyShadowWorkGroup(const WorkGroup *workGroup)
{
    assert(m_workSpace.workGroups->count(workGroup) && "No shadow for workgroup found!");
    delete (*m_workSpace.workGroups)[workGroup];
    m_workSpace.workGroups->erase(workGroup);
}

void ShadowContext::dump(const WorkItem *workItem) const
{
    dumpGlobalValues();
    m_globalMemory->dump();
    if(m_workSpace.workGroups && m_workSpace.workGroups->size())
    {
        m_workSpace.workGroups->begin()->second->dump();
    }
    if(m_workSpace.workItems && m_workSpace.workItems->size())
    {
        if(workItem)
        {
            cout << "Item " << workItem->getGlobalID() << endl;
            getShadowWorkItem(workItem)->dump();
        }
        else
        {
            ShadowItemMap::const_iterator itr;
            for(itr = m_workSpace.workItems->begin(); itr != m_workSpace.workItems->end(); ++itr)
            {
                cout << "Item " << itr->first->getGlobalID() << endl;
                itr->second->dump();
            }
        }
    }
}

void ShadowContext::dumpGlobalValues() const
{
    cout << "==== ShadowMap (global) =======" << endl;

    UnorderedIntervalMap::const_iterator itr;
    unsigned num = 1;

    for(itr = m_globalValues.begin(); itr != m_globalValues.end(); ++itr)
    {
        if(itr->first->hasName())
        {
            cout << "%" << itr->first->getName().str() << ": " << itr->second << endl;
        }
        else
        {
            cout << "%" << dec << num++ << ": " << itr->second << endl;
        }
    }

    cout << "=======================" << endl;
}

void ShadowContext::freeWorkItems()
{
    if(m_workSpace.workItems && !m_workSpace.workItems->size())
    {
        delete m_workSpace.workItems;
        m_workSpace.workItems = NULL;
    }
}

void ShadowContext::freeWorkGroups()
{
    if(m_workSpace.workGroups && !m_workSpace.workGroups->size())
    {
        delete m_workSpace.workGroups;
        m_workSpace.workGroups = NULL;
    }
}

/*
TypedValue ShadowContext::getCleanValue(unsigned size)
{
    TypedValue v = {
        size,
        1,
        m_workSpace.memoryPool->alloc(size)
    };

    memset(v.data, 0, size);

    return v;
}

TypedValue ShadowContext::getCleanValue(TypedValue v)
{
    TypedValue c = {
        v.size,
        v.num,
        m_workSpace.memoryPool->alloc(v.size*v.num)
    };

    memset(c.data, 0, v.size*v.num);

    return c;
}

TypedValue ShadowContext::getCleanValue(const llvm::Value *V)
{
    pair<unsigned,unsigned> size = getValueSize(V);
    TypedValue v = {
        size.first,
        size.second,
        m_workSpace.memoryPool->alloc(size.first*size.second)
    };

    memset(v.data, 0, v.size*v.num);

    return v;
}
*/

//for float_test


Interval* ShadowContext::getIntervalFromFloat(float f){

	Interval* inter = new Interval(f);

	return inter;
}



// for float_test
Interval* ShadowContext::getUninitializedInterval()
{
    Interval* inter = new Interval(-INF, INF);

    return inter;
}

Interval* ShadowContext::getValue(const WorkItem *workItem, const llvm::Value *V) const
{
    if(m_globalValues.count(V))
    {
        return m_globalValues.at(V);
    }
    else
    {
        ShadowValues *shadowValues = getShadowWorkItem(workItem)->getValues();
        return shadowValues->getValue(V);
    }
}


void ShadowContext::setGlobalValue(const llvm::Value *V, Interval* inter)
{
    assert(!m_globalValues.count(V) && "Values may only have one shadow");
    m_globalValues[V] = inter;
}