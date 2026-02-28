#pragma once
// ============================================================
//  MemoryReassembly.h
// ============================================================
#include "NetworkFuser.h"

class MemoryReassembly
{
public:
    MemoryReassembly();
    ~MemoryReassembly();

    // Feed one raw UDP payload (including header).
    // Returns a pointer to a completed FrameSlot on frame completion,
    // nullptr otherwise. The pointer is valid until the next call.
    FrameSlot* ConsumePacket(const uint8_t* rawData, int rawLen);

    // Call periodically to evict stale incomplete frames
    void PurgeExpired();

    // After consuming the completed frame, reset it so the slot can be reused
    void ReleaseSlot(FrameSlot* slot) { if (slot) ResetSlot(*slot); }

private:
    FrameSlot* FindOrAllocSlot(uint32_t frameID, uint32_t totalPackets);
    void       ResetSlot(FrameSlot& s);

    std::vector<FrameSlot> m_slots;  // fixed-size ring
};
