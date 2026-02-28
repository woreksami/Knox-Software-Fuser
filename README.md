# Knox Fuser (Software DMA Fuser via UDP)

## 📌 Project Overview
Knox Fuser is a completely software-based zero-latency ESP fuser meant to mimic the behavior of a physical DMA hardware fuser. The goal is to stream an uncompressed, raw surface/memory overlay from a Second PC (Sender) across a direct copper Ethernet cable to the Main PC (Receiver) using raw UDP sockets. 

Unlike Moonlight, Sunshine, or NDI which rely heavily on H264/HEVC video encoding (introducing a mandated 10-25ms delay), Knox Fuser pulls raw `BGRA` pixel data from an overlay, blasts it over a direct connection entirely uncompressed, and uses a custom packet reassembly queue to instantly draw that frame onto a hijacked overlay `DwmExtendFrameIntoClientArea` structure on the Main PC.

If configured perfectly on a clean Windows machine, the latency from frame capture on PC2 to overlay render on PC1 is designed to be under 2 milliseconds.

## 🚨 Current State / Why It's Broken
The C++ logic (Memory Reassembly, Direct3D capturing, ImGui overlay hijacking, and UDP Socket reading) is fully complete and compiled correctly. The protocol works flawlessly on `localhost` loopback tests.

**The issue is Windows Network Routing.**
When testing across a direct Ethernet cable (`10.0.0.x` or `169.254.1.x`) on dual-NIC systems (where the Second PC also has a WiFi connection to the internet), Windows aggressively hijacks the routing table and attempts to route the raw UDP packets out the active internet connection instead of the direct cable, causing 100% video loss at the Sender side, or dropping it at the Receiver side via strict Firewall profiles (`Unidentified Network / Public Profile` drop issues).

Despite attempts to force `INADDR_ANY`, explicit `10.0.0.2` binding, and PowerShell link-local routing rules, the networking environment continues to drop sockets.

## 🤝 Call for Help (To Other Devs)
**We are open-sourcing this right now to find someone who understands deep Windows Socket Subnets, NDIS kernel routing, or low-level dual-NIC packet handling.**

We need help fixing the `Sender.cpp` and `Receiver.cpp` Networking implementations so it seamlessly broadcasts across the direct-attached Ethernet cable WITHOUT Windows stepping in to block/redirect the packets out the main WiFi adapter. 

### What needs fixing:
1. **Routing:** How can we ensure a raw UDP `sendto` call over a specific ethernet interface bypasses standard Windows internet default-route hijacking? (Do we need raw sockets? WinPcap/Npcap?)
2. **Firewalls:** Is there a way for a user-mode application to cleanly bypass the "Unidentified Network" firewall drop without forcing the user to globally disable Defender?
3. **Packet Reliability:** Currently uses a naive `BEEP` heartbeat and uncompressed packet slicing. What's the cleanest way to ensure data persistence over a noisy copper cable without TCP overhead?

## 🛠️ Instructions to Build
1. Open `KnoxFuser.sln` in Visual Studio 2022.
2. Ensure you are building on x64 Release mode.
3. Contains dependencies: `ImGui`, `DirectX 11`, `Winsock2`.

## ⚙️ How it works
* Run `KnoxFuser.exe` on Main PC. Click `Receiver`.
* Run `KnoxFuser.exe` on Second PC. Click `Sender`.
* You can explicitly type the remote IP into the `Sender` UI to force a target, or leave it blank to attempt a global local broadcast discovery.
