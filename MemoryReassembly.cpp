// ============================================================
//  MemoryReassembly.cpp  –  Ring-buffer frame reassembly
//  Zero-Latency Network Video Fuser
// ============================================================

#include "NetworkFuser.h"
#include "MemoryReassembly.h"

// ─────────────────────────────────────────────────────────────
MemoryReassembly::MemoryReassembly()
{
    m_slots.resize(REASSEMBLY_SLOTS);
}

MemoryReassembly::~MemoryReassembly() = default;

// ─── Feed one UDP packet into the reassembly engine ─────────
//  Returns pointer to completed FrameSlot (owned by ring buffer,
//  valid until the next call that reuses the same slot),
//  or nullptr if the frame is not yet complete.
FrameSlot* MemoryReassembly::ConsumePacket(const uint8_t* rawData, int rawLen)
{
    if (rawLen < static_cast<int>(HEADER_SIZE))
        return nullptr;

    // Parse header
    FuserPacketHeader hdr;
    std::memcpy(&hdr, rawData, HEADER_SIZE);
    const uint8_t* payload    = rawData    + HEADER_SIZE;
    const int      payloadLen = rawLen     - static_cast<int>(HEADER_SIZE);

    if (hdr.TotalPackets == 0 || hdr.PacketIndex >= hdr.TotalPackets)
        return nullptr;

    // Locate or allocate a slot for this FrameID
    FrameSlot* slot = FindOrAllocSlot(hdr.FrameID, hdr.TotalPackets);
    if (!slot)
        return nullptr;

    // Ignore duplicate packets
    if (slot->received[hdr.PacketIndex])
        return nullptr;

    // Packet 0 carries the FrameMetaPayload before pixel data
    if (hdr.PacketIndex == 0)
    {
        if (payloadLen < static_cast<int>(FRAME_META_SIZE))
            return nullptr;

        FrameMetaPayload meta;
        std::memcpy(&meta, payload, FRAME_META_SIZE);

        slot->width      = meta.width;
        slot->height     = meta.height;
        slot->totalBytes = meta.rawBytes;

        // Resize pixel buffer once we know the frame size
        if (slot->pixelData.size() != meta.rawBytes)
            slot->pixelData.resize(meta.rawBytes, 0);

        // Copy pixel portion of packet 0
        const int pixelLen = payloadLen - static_cast<int>(FRAME_META_SIZE);
        if (pixelLen > 0)
        {
            uint32_t toCopy = static_cast<uint32_t>(pixelLen);
            if (toCopy > meta.rawBytes) toCopy = meta.rawBytes;
            std::memcpy(slot->pixelData.data(), payload + FRAME_META_SIZE, toCopy);
        }
    }
    else
    {
        // Packets 1..N: locate write offset.
        // Offset = pixelBytesInPkt0 + (pktIdx-1)*MAX_PIXEL_PAYLOAD
        constexpr uint32_t pixelBytesInPkt0 = MAX_PIXEL_PAYLOAD - FRAME_META_SIZE;
        const uint32_t writeOffset =
            pixelBytesInPkt0 +
            (static_cast<uint32_t>(hdr.PacketIndex) - 1) * MAX_PIXEL_PAYLOAD;

        // Guard against buffer overflow (malformed packet)
        if (slot->pixelData.empty())
            return nullptr; // metadata packet 0 not arrived yet; discard
        if (writeOffset >= slot->totalBytes)
            return nullptr;

        uint32_t toCopy = static_cast<uint32_t>(payloadLen);
        if (writeOffset + toCopy > slot->totalBytes)
            toCopy = slot->totalBytes - writeOffset;

        std::memcpy(slot->pixelData.data() + writeOffset, payload, toCopy);
    }

    slot->received[hdr.PacketIndex] = true;
    ++slot->receivedCount;

    // Check for frame timeout – drop incomplete frames to maintain low latency
    const ULONGLONG now = FuserUtil::NowMs();
    if (now - slot->firstPacketMs > FRAME_TIMEOUT_MS)
    {
        // Too old – evict slot silently
        ResetSlot(*slot);
        return nullptr;
    }

    // Frame complete?
    if (slot->receivedCount == slot->totalPackets)
    {
        slot->complete = true;
        return slot;
    }

    return nullptr;
}

// ─── Evict all slots older than FRAME_TIMEOUT_MS ────────────
void MemoryReassembly::PurgeExpired()
{
    const ULONGLONG now = FuserUtil::NowMs();
    for (auto& s : m_slots)
    {
        if (s.frameID != 0 && !s.complete)
        {
            if (now - s.firstPacketMs > FRAME_TIMEOUT_MS)
                ResetSlot(s);
        }
    }
}

// ─── Find an existing slot for frameID, or allocate a new one ─
FrameSlot* MemoryReassembly::FindOrAllocSlot(uint32_t frameID,
                                              uint32_t totalPackets)
{
    // Search for existing slot
    for (auto& s : m_slots)
    {
        if (s.frameID == frameID)
            return &s;
    }

    // Find a free (unused or complete/expired) slot
    FrameSlot* victim = nullptr;
    ULONGLONG  oldest = ULLONG_MAX;

    for (auto& s : m_slots)
    {
        if (s.frameID == 0 || s.complete)
        {
            ResetSlot(s);
            victim = &s;
            break;
        }
        // Track oldest incomplete slot as fallback eviction target
        if (s.firstPacketMs < oldest)
        {
            oldest = s.firstPacketMs;
            victim = &s;
        }
    }

    if (!victim)
        return nullptr;

    // Initialise the slot
    ResetSlot(*victim);
    victim->frameID      = frameID;
    victim->totalPackets = totalPackets;
    victim->received.assign(totalPackets, false);
    victim->firstPacketMs = FuserUtil::NowMs();

    return victim;
}

void MemoryReassembly::ResetSlot(FrameSlot& s)
{
    s.frameID      = 0;
    s.totalPackets = 0;
    s.receivedCount= 0;
    s.totalBytes   = 0;
    s.complete     = false;
    s.firstPacketMs= 0;
    s.width        = 0;
    s.height       = 0;
    // Don't release the memory – keep capacity for reuse
    if (!s.received.empty())  s.received.assign(s.received.size(), false);
}
