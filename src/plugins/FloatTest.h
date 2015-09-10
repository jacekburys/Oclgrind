// FloatTest.h (Oclgrind)
// Copyright (c) 2015, Moritz Pflanzer
// Imperial College London. All rights reserved.
//
// Based on MemCheckUninitialized plugin by Moritz Pflanzer
//
// This program is provided under a three-clause BSD license. For full
// license terms please see the LICENSE file distributed with this
// source code.

#include "core/Plugin.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IntrinsicInst.h"
#include <boost/numeric/interval.hpp>
#include <limits>

//#define DUMP_SHADOW
//#define PARANOID_CHECK(W, I) assert(checkAllOperandsDefined(W, I) && "Not all operands defined")
//#define PARANOID_CHECK(W, I) checkAllOperandsDefined(W, I)
#define PARANOID_CHECK(W, I) (void*)0

namespace oclgrind
{
	typedef boost::numeric::interval<float> Interval;
    typedef std::unordered_map<const llvm::Value*, Interval*> UnorderedIntervalMap;
    const float INF = std::numeric_limits<float>::infinity();

    class ShadowFrame
    {
        public:
            ShadowFrame();
            virtual ~ShadowFrame();

            void dump() const;
            inline const llvm::CallInst* getCall() const
            {
                return m_call;
            }
            Interval* getValue(const llvm::Value *V) const;
            inline bool hasValue(const llvm::Value* V) const
            {
                return llvm::isa<llvm::Constant>(V) || m_values->count(V);
            }
            inline void setCall(const llvm::CallInst *CI)
            {
                m_call = CI;
            }
            void setValue(const llvm::Value *V, Interval* inter);

        private:
            typedef std::list<const llvm::Value*> ValuesList;

            const llvm::CallInst *m_call;
            UnorderedIntervalMap *m_values;
#ifdef DUMP_SHADOW
            ValuesList *m_valuesList;
#endif
    };

    class ShadowValues
    {
        public:
            ShadowValues();
            virtual ~ShadowValues();

            ShadowFrame* createCleanShadowFrame();
            inline void dump() const
            {
                m_stack->top()->dump();
            }
            inline const llvm::CallInst* getCall() const
            {
                return m_stack->top()->getCall();
            }
            inline Interval* getValue(const llvm::Value *V) const
            {
                return m_stack->top()->getValue(V);
            }
            inline bool hasValue(const llvm::Value* V) const
            {
                return llvm::isa<llvm::Constant>(V) || m_stack->top()->hasValue(V);
            }
            inline void popFrame()
            {
                m_stack->pop();
            }
            inline void pushFrame(ShadowFrame *frame)
            {
                m_stack->push(frame);
            }
            inline void setCall(const llvm::CallInst *CI)
            {
                m_stack->top()->setCall(CI);
            }
            inline void setValue(const llvm::Value *V, Interval* inter)
            {
                m_stack->top()->setValue(V, inter);
            }

        private:
            typedef std::stack<ShadowFrame*> ShadowValuesStack;

            ShadowValuesStack *m_stack;
    };

    class ShadowMemory
    {
        public:
    	/*
            struct Buffer
            {
                size_t size;
                cl_mem_flags flags;
                unsigned char *data;
            };
		*/


            ShadowMemory(AddressSpace addrSpace, unsigned bufferBits);
            virtual ~ShadowMemory();

            void allocate(size_t address);
            void dump() const;
            void* getPointer(size_t address) const;
            bool isAddressValid(size_t address) const;
            Interval* load(size_t address) const;
            void lock(size_t address) const;
            void store(Interval *inter, size_t address);
            void unlock(size_t address) const;

        private:


            typedef std::unordered_map<size_t, Interval* /*Buffer**/> MemoryMap;

            AddressSpace m_addrSpace;
            MemoryMap m_map;
            unsigned m_numBitsAddress;
            unsigned m_numBitsBuffer;

            void clear();
            void deallocate(size_t address);
            size_t extractBuffer(size_t address) const;
            size_t extractOffset(size_t address) const;
    };

    class ShadowWorkItem
    {
        public:
            ShadowWorkItem(unsigned bufferBits);
            virtual ~ShadowWorkItem();

            inline void dump() const
            {
                m_values->dump();
                m_memory->dump();
            }
            inline ShadowMemory* getPrivateMemory()
            {
                return m_memory;
            }
            inline ShadowValues* getValues() const
            {
                return m_values;
            }

        private:
            ShadowMemory *m_memory;
            ShadowValues *m_values;
    };

    class ShadowWorkGroup
    {
        public:
            ShadowWorkGroup(unsigned bufferBits);
            virtual ~ShadowWorkGroup();

            inline void dump() const
            {
                m_memory->dump();
            }
            inline ShadowMemory* getLocalMemory()
            {
                return m_memory;
            }

        private:
            ShadowMemory *m_memory;
    };

    class ShadowContext
    {
        public:
            ShadowContext(unsigned bufferBits);
            virtual ~ShadowContext();

            void allocateWorkItems();
            void allocateWorkGroups();
            void createMemoryPool();
            ShadowWorkItem* createShadowWorkItem(const WorkItem *workItem);
            ShadowWorkGroup* createShadowWorkGroup(const WorkGroup *workGroup);
            void destroyMemoryPool();
            void destroyShadowWorkItem(const WorkItem *workItem);
            void destroyShadowWorkGroup(const WorkGroup *workGroup);
            void dump(const WorkItem *workItem) const;
            void dumpGlobalValues() const;
            void freeWorkItems();
            void freeWorkGroups();

            //for float test
            static Interval* getUninitializedInterval();
            static Interval* getIntervalFromFloat(float f);
            static int64_t intervalToInt(Interval* inter);
            static bool isnan(Interval inter);


            inline ShadowMemory* getGlobalMemory() const
            {
                return m_globalMemory;
            }
            TypedValue getGlobalValue(const llvm::Value *V) const;
            MemoryPool* getMemoryPool() const
            {
                return m_workSpace.memoryPool;
            }

            inline ShadowWorkItem* getShadowWorkItem(const WorkItem *workItem) const
            {
                return m_workSpace.workItems->at(workItem);
            }
            inline ShadowWorkGroup* getShadowWorkGroup(const WorkGroup *workGroup) const
            {
                return m_workSpace.workGroups->at(workGroup);
            }
            Interval* getValue(const WorkItem *workItem, const llvm::Value *V) const;
            inline bool hasValue(const WorkItem *workItem, const llvm::Value* V) const
            {
                return llvm::isa<llvm::Constant>(V) || m_globalValues.count(V) || m_workSpace.workItems->at(workItem)->getValues()->hasValue(V);
            }
            /*
            static bool isCleanStruct(ShadowMemory *shadowMemory, size_t address, const llvm::StructType *structTy);
            static bool isCleanValue(TypedValue v);
            static bool isCleanValue(TypedValue v, unsigned offset);
            */
            void setGlobalValue(const llvm::Value *V, Interval* inter);

        private:
            ShadowMemory *m_globalMemory;
            UnorderedIntervalMap m_globalValues;
            unsigned m_numBitsBuffer;
            typedef std::map<const WorkItem*, ShadowWorkItem*> ShadowItemMap;
            typedef std::map<const WorkGroup*, ShadowWorkGroup*> ShadowGroupMap;
            struct WorkSpace
            {
                ShadowItemMap *workItems;
                ShadowGroupMap *workGroups;
                MemoryPool *memoryPool;
                unsigned poolUsers;
            };
            static THREAD_LOCAL WorkSpace m_workSpace;
    };

    class FloatTest : public Plugin
    {
        public:
            FloatTest(const Context *context);
            virtual ~FloatTest();

            virtual void hostMemoryStore(const Memory *memory,
                    size_t address, size_t size,
                    const uint8_t *storeData) override;
            virtual void instructionExecuted(const WorkItem *workItem,
                    const llvm::Instruction *instruction,
                    const TypedValue& result) override;
            virtual void kernelBegin(const KernelInvocation *kernelInvocation) override;
            virtual void memoryMap(const Memory *memory, size_t address,
                    size_t offset, size_t size, cl_map_flags flags) override;
            virtual void workItemBegin(const WorkItem *workItem) override;
            virtual void workItemComplete(const WorkItem *workItem) override;
            virtual void workGroupBegin(const WorkGroup *workGroup) override;
            virtual void workGroupComplete(const WorkGroup *workGroup) override;
            //virtual void memoryAllocated(const Memory *memory, size_t address,
            //                             size_t size, cl_mem_flags flags,
            //                             const uint8_t *initData);
        private:
            std::list<std::pair<const llvm::Value*, TypedValue> > m_deferredInit;
            std::list<std::pair<const llvm::Value*, TypedValue> > m_deferredInitGroup;
            ShadowContext shadowContext;
            //MemoryPool m_pool;

            void allocAndStoreShadowMemory(unsigned addrSpace, size_t address, Interval* inter,
                                           const WorkItem *workItem = NULL, const WorkGroup *workGroup = NULL, bool unchecked = false);
            //bool checkAllOperandsDefined(const WorkItem *workItem, const llvm::Instruction *I);
            void checkStructMemcpy(const WorkItem *workItem, const llvm::Value *src);
            //void copyShadowMemory(unsigned dstAddrSpace, size_t dst,
            //                      unsigned srcAddrSpace, size_t src, unsigned size,
            //                      const WorkItem *workItem = NULL, const WorkGroup *workGroup = NULL, bool unchecked = false);
            //void copyShadowMemoryStrided(unsigned dstAddrSpace, size_t dst,
            //                            unsigned srcAddrSpace, size_t src,
            //                             size_t num, size_t stride, unsigned size,
            //                             const WorkItem *workItem = NULL, const WorkGroup *workGroup = NULL, bool unchecked = false);
            static std::string extractUnmangledName(const std::string fullname);
            ShadowMemory* getShadowMemory(unsigned addrSpace, const WorkItem *workItem = NULL, const WorkGroup *workGroup = NULL) const;
            bool handleBuiltinFunction(const WorkItem *workItem, std::string name, const llvm::CallInst *CI, const TypedValue result);
            void handleIntrinsicInstruction(const WorkItem *workItem, const llvm::IntrinsicInst *I);
            Interval* loadShadowMemory(unsigned addrSpace, size_t address,
                                  const WorkItem *workItem = NULL, const WorkGroup *workGroup = NULL);
            void logUninitializedAddress(unsigned int addrSpace, size_t address, bool write = true) const;
            void logUninitializedCF() const;
            void logUninitializedIndex() const;
            void logUninitializedWrite(unsigned int addrSpace, size_t address) const;
            //void SimpleOr(const WorkItem *workItem, const llvm::Instruction *I);
            void simpleFloatInstruction(const WorkItem *workItem, const llvm::Instruction *instruction);
            void handleCmpInstruction(const WorkItem *workItem, const llvm::Instruction *instruction, const TypedValue& result);
            //void SimpleOrAtomic(const WorkItem *workItem, const llvm::CallInst *CI);
            void storeShadowMemory(unsigned addrSpace, size_t address, Interval* inter,
                                   const WorkItem *workItem = NULL, const WorkGroup *workGroup = NULL, bool unchecked = false);
    };
}
