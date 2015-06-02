// Uninitialized.h (Oclgrind)
// Copyright (c) 2013-2015, James Price and Simon McIntosh-Smith,
// University of Bristol. All rights reserved.
//
// This program is provided under a three-clause BSD license. For full
// license terms please see the LICENSE file distributed with this
// source code.

#include "core/Plugin.h"

namespace oclgrind
{
  class Uninitialized : public Plugin
  {
  public:
    Uninitialized(const Context *context);

    virtual void hostMemoryStore(const Memory *memory,
                                 size_t address, size_t size,
                                 const uint8_t *storeData) override;
    virtual void memoryAllocated(const Memory *memory, size_t address,
                                 size_t size, cl_mem_flags flags) override;
    virtual void memoryAtomicLoad(const Memory *memory,
                                  const WorkItem *workItem,
                                  AtomicOp op,
                                  size_t address, size_t size) override;
    virtual void memoryAtomicStore(const Memory *memory,
                                   const WorkItem *workItem,
                                   AtomicOp op,
                                   size_t address, size_t size) override;
    virtual void memoryDeallocated(const Memory *memory, size_t address);
    virtual void memoryLoad(const Memory *memory, const WorkItem *workItem,
                            size_t address, size_t size) override;
    virtual void memoryLoad(const Memory *memory, const WorkGroup *workGroup,
                            size_t address, size_t size) override;
    virtual void memoryStore(const Memory *memory, const WorkItem *workItem,
                             size_t address, size_t size,
                             const uint8_t *storeData) override;
    virtual void memoryStore(const Memory *memory, const WorkGroup *workGroup,
                             size_t address, size_t size,
                             const uint8_t *storeData) override;

  private:
    typedef std::map< std::pair<const Memory*, size_t>, bool* > StateMap;
    StateMap m_state;

    void checkState(const Memory *memory, size_t address, size_t size) const;
    void setState(const Memory *memory, size_t address, size_t size);

    void logError(unsigned int addrSpace, size_t address) const;
  };
}
